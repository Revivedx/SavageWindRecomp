#!/usr/bin/env python3
"""Check the shadow cascade draw budget (cpuopts bit 512, exec.c shadow_budget_skip / shadow_budget_choose).

The real shadow_budget_choose and shadow_budget_skip are extracted from exec.c and driven with random frames: passes into
the cascade image (layers in a random order, a clear and draws each, several frames apart or on the same frame), passes
that are not a cascade (a color target, a depth image of another shape), and a new image appearing. A model written here,
not taken from the code, says what must be drawn:
  - a layer outside the set (SAVAGEWIND_SHADOW_BUDGET_LAYERS) is always drawn,
  - a layer in the set is drawn when it was not drawn in one of the last (PERIOD - 1) frames, and otherwise left out,
  - the answer is the same for the clear and every draw of a layer in a frame,
  - a new image starts with every layer drawn, a pass that is no cascade is never touched,
  - a left-out layer's image has this frame noted as drawn, the counters count the draws and clears of each outcome.
It runs once for each of several environments (layers 4, 7, 5, 0, 1, period 1, 2, 3, 8).

    tools/test_shadow_budget.py [--exec runtime/nvn/vulkan/exec.c]
    tools/test_shadow_budget.py --self-check    (breaks the code in several ways; each must be caught)
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def function(source, name):
    match = re.search(r'^static [^;{}]*?\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', source, re.M)
    brace = match.end() - 1
    depth, pos = 1, brace + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


def line(source, prefix):
    i = source.index(prefix)
    return source[i:source.index('\n', i)]


FIXTURE = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) ((void)0)
typedef struct {uint64_t drawn_frame, prev_drawn, serial;} TexBackend;
typedef struct {int target, depth, flags, width, height, format; TexBackend *backend;} NvnTexture;
typedef struct {uint32_t setmask, min_layer;} View;
static struct {struct {int ncolor; NvnTexture *depth; uint8_t has_dview; View dview;} rt;} st;
static int opts_value;
static int stage_cpu_opts(void) {return opts_value;}
static uint64_t present;
static uint64_t renderer_present_count(void) {return present;}
static bool renderer_format_is_depth(uint32_t f) {return f==7 || f==8;}
static void stat_bump(_Atomic uint64_t *c) {atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) + 1, memory_order_relaxed);}
@DECLS@
@FUNCS@

static uint32_t rng=2463534242u;
static uint32_t next(void) {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}

static uint32_t want_layers=4, want_period=2;
typedef struct {bool seen; uint64_t frame;} Last;
static Last model[8];
static uint64_t model_serial;
static long m_skip_draws, m_skip_clears, m_skip_passes, m_drawn[8], m_cleared[8];
static bool decided_frame_skip[8]; static uint64_t decided_frame[8];

static NvnTexture cascade={4,3,8,896,896,7,NULL}, other={4,3,8,896,896,7,NULL};
static TexBackend be_a, be_b;

static void cascade_state(NvnTexture *t,TexBackend *b,int layer) {
    memset(&st,0,sizeof st);
    st.rt.depth=t; t->backend=b; st.rt.has_dview=1; st.rt.dview.setmask=2; st.rt.dview.min_layer=layer;
}

// one call (a clear or a draw) of the pass for a layer, checked against the model
static void call(TexBackend *b,int layer,bool is_draw,bool fresh_image) {
    uint64_t frame=present;
    if(fresh_image) {model_serial=b->serial; memset(model,0,sizeof model); memset(decided_frame,0,sizeof decided_frame);}
    bool in_set=want_layers>>layer&1;
    if(decided_frame[layer]!=frame+1) {
        decided_frame[layer]=frame+1;
        Last *l=&model[layer];
        decided_frame_skip[layer]=in_set && l->seen && frame-l->frame<want_period;
        if(decided_frame_skip[layer]) m_skip_passes++; else {l->seen=true; l->frame=frame;}
    }
    bool want=decided_frame_skip[layer];
    uint64_t before_drawn=b->drawn_frame;
    cascade_state(&cascade,b,layer);
    bool got=shadow_budget_skip(is_draw);
    if(got!=want) {
        printf("FAIL: frame %llu layer %d %s: %s, expected %s (layers %#x, every %u)\n",(unsigned long long)frame,layer,is_draw?"draw":"clear",
               got?"left out":"drawn",want?"left out":"drawn",want_layers,want_period);
        exit(1);
    }
    if(want) {
        if(is_draw) m_skip_draws++; else m_skip_clears++;
        if(b->drawn_frame!=frame || (before_drawn!=frame && b->prev_drawn!=before_drawn)) {printf("FAIL: a left-out layer's image: drawn %llu prev %llu\n",(unsigned long long)b->drawn_frame,(unsigned long long)b->prev_drawn); exit(1);}
    } else {
        if(b->drawn_frame!=before_drawn) {printf("FAIL: a drawn layer changed the image's frame notes\n"); exit(1);}
        (is_draw ? m_drawn : m_cleared)[layer]++;
    }
}

int main(void) {
    const char *e=getenv("SAVAGEWIND_SHADOW_BUDGET_LAYERS");
    want_layers=e ? (uint32_t)strtoul(e,NULL,0)&0xff : 4;
    e=getenv("SAVAGEWIND_SHADOW_BUDGET_PERIOD");
    want_period=e ? (uint32_t)strtoul(e,NULL,0) : 2;
    if(want_period<1) want_period=1;
    if(want_period>8) want_period=8;
    be_a.serial=11; be_b.serial=12;
    TexBackend *cur=&be_a;
    long passes=0, left_out=0, others=0;
    opts_value=512;
    for(int frame=0;frame<30000;frame++) {
        int r=next()%100;
        present+= r<80 ? 1 : (r<90 ? 0 : (r<97 ? 2+next()%3 : 1+next()%40));  // (the same frame again, a few frames, a jump)
        bool fresh=false;
        if(next()%200==0) {cur=cur==&be_a ? &be_b : &be_a; fresh=true; cur->serial=100+next()%1000000;}  // (a new image)
        // the frame's passes: the layers in a random order, each a clear and draws (some frames only draws, as a layer that does not clear)
        int order[3]={0,1,2};
        for(int i=2;i>0;i--) {int j=next()%(i+1); int t=order[i]; order[i]=order[j]; order[j]=t;}
        for(int k=0;k<3;k++) {
            if(next()%10==0) continue;  // (a frame without that layer)
            int layer=order[k];
            bool first=true;
            if(next()%8) {call(cur,layer,false,fresh); fresh=false; first=false;}
            int draws=next()%5==0 ? 0 : 1+next()%12;
            for(int d=0;d<draws;d++) {call(cur,layer,true,fresh); fresh=false; first=false;}
            (void)first;
            passes++;
            // a pass between that is no cascade, and must not be touched or counted
            if(next()%3==0) {
                memset(&st,0,sizeof st);
                int kind=next()%7;
                NvnTexture t=other; TexBackend b={0,0,7};
                t.backend=&b; st.rt.depth=&t; st.rt.has_dview=1; st.rt.dview.setmask=2; st.rt.dview.min_layer=next()%3;
                switch(kind) {
                case 0: st.rt.ncolor=1; break;
                case 1: t.target=1; break;
                case 2: t.depth=1; break;
                case 3: t.flags=0; break;
                case 4: t.width=895; break;
                case 5: t.width=256; t.height=256; break;
                case 6: t.format=3; break;
                }
                uint64_t skips=m_skip_draws+m_skip_clears;
                (void)skips;
                if(shadow_budget_skip(next()&1)) {printf("FAIL: a pass of kind %d was left out\n",kind); exit(1);}
                if(b.drawn_frame||b.prev_drawn) {printf("FAIL: a pass of kind %d changed an image\n",kind); exit(1);}
                others++;
            }
            if(next()%50==0) {  // a layer out of the range the table keeps, or no image behind the target: never touched
                memset(&st,0,sizeof st);
                st.rt.depth=&cascade; cascade.backend=NULL; st.rt.has_dview=1; st.rt.dview.setmask=2; st.rt.dview.min_layer=1;
                if(shadow_budget_skip(true)) {printf("FAIL: a target without an image was left out\n"); exit(1);}
                cascade_state(&cascade,cur,9);
                if(shadow_budget_skip(true)) {printf("FAIL: layer 9 was left out\n"); exit(1);}
            }
        }
    }
    left_out=m_skip_draws+m_skip_clears;
    if(want_period==1 || !(want_layers&7)) {if(left_out) {printf("FAIL: %ld calls left out with nothing to leave out\n",left_out); exit(1);}}
    else if(left_out<1000 || (m_drawn[0]+m_drawn[1]+m_drawn[2])<1000) {printf("FAIL: the run does not exercise both outcomes (%ld left out)\n",left_out); exit(1);}
    // the counters
    if(atomic_load(&sb_skip_draws)!=(uint64_t)m_skip_draws || atomic_load(&sb_skip_clears)!=(uint64_t)m_skip_clears ||
       atomic_load(&sb_skip_passes)!=(uint64_t)m_skip_passes) {
        printf("FAIL: counters: left out %llu draws %llu clears %llu passes, model %ld %ld %ld\n",(unsigned long long)atomic_load(&sb_skip_draws),
               (unsigned long long)atomic_load(&sb_skip_clears),(unsigned long long)atomic_load(&sb_skip_passes),m_skip_draws,m_skip_clears,m_skip_passes);
        return 1;
    }
    for(int i=0;i<3;i++) if(atomic_load(&sb_drawn[i])!=(uint64_t)m_drawn[i] || atomic_load(&sb_cleared[i])!=(uint64_t)m_cleared[i]) {
        printf("FAIL: counters of layer %d: drawn %llu cleared %llu, model %ld %ld\n",i,(unsigned long long)atomic_load(&sb_drawn[i]),
               (unsigned long long)atomic_load(&sb_cleared[i]),m_drawn[i],m_cleared[i]);
        return 1;
    }
    printf("ok: layers %#x every %u frames: %ld layer passes, %ld draws and %ld clears left out, %ld other passes untouched\n",
           want_layers,want_period,passes,m_skip_draws,m_skip_clears,others);
    return 0;
}
'''


def build(src, mutate=None):
    decls = [line(src, '#define SB_LAYERS')]
    start = src.index('static struct {\n    uint64_t serial;')
    decls.append(src[start:src.index('} sb;', start) + len('} sb;')])
    decls.append(line(src, 'static _Atomic uint64_t sb_skip_draws'))
    funcs = [function(src, n) for n in ('shadow_budget_choose', 'shadow_budget_skip')]
    text = FIXTURE.replace('@DECLS@', '\n'.join(decls)).replace('@FUNCS@', '\n'.join(funcs))
    if mutate:
        old, new = mutate
        a = text.index('static bool shadow_budget_choose')
        b = text.index('static uint32_t rng')
        assert text[a:b].count(old) == 1, old
        text = text[:a] + text[a:b].replace(old, new, 1) + text[b:]
    return text


ENVS = [{}, {'SAVAGEWIND_SHADOW_BUDGET_LAYERS': '7'}, {'SAVAGEWIND_SHADOW_BUDGET_LAYERS': '5', 'SAVAGEWIND_SHADOW_BUDGET_PERIOD': '3'},
        {'SAVAGEWIND_SHADOW_BUDGET_LAYERS': '0'}, {'SAVAGEWIND_SHADOW_BUDGET_LAYERS': '1', 'SAVAGEWIND_SHADOW_BUDGET_PERIOD': '8'},
        {'SAVAGEWIND_SHADOW_BUDGET_PERIOD': '1'}, {'SAVAGEWIND_SHADOW_BUDGET_LAYERS': '0xff', 'SAVAGEWIND_SHADOW_BUDGET_PERIOD': '2'},
        {'SAVAGEWIND_SHADOW_BUDGET_PERIOD': '0'}, {'SAVAGEWIND_SHADOW_BUDGET_PERIOD': '99'}]


def run(text, tmp, envs, quiet=False):
    c = Path(tmp) / 't.c'
    c.write_text(text)
    exe = Path(tmp) / 't'
    r = subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-w', str(c), '-o', str(exe)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr)
    for extra in envs:
        env = {k: v for k, v in os.environ.items() if not k.startswith('SAVAGEWIND_')}
        env.update(extra, ASAN_OPTIONS='detect_leaks=0')
        r = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
        if r.returncode:
            if not quiet:
                print(r.stdout + r.stderr)
            return False
        if not quiet:
            print(r.stdout.strip())
    return True


MUTANTS = [
    ('skips one frame too many', 'frame + 1 - *last < period', 'frame + 1 - *last <= period'),
    ('skips one frame too few', 'frame + 1 - *last < period', 'frame + 1 - *last < period - 1'),
    ('leaves out layers outside the set', 'if (in_set && *last &&', 'if (*last &&'),
    ('leaves out a layer never drawn', 'if (in_set && *last &&', 'if (in_set &&'),
    ('never notes a drawn frame', '*last = frame + 1;', ''),
    ('notes the frame on a skip too', 'return true;\n    *last = frame + 1;', '{ *last = frame + 1; return true; }\n    *last = frame + 1;'),
    ('forgets the image changed', 'if (sb.serial != b->serial) {', 'if (0) {'),
    ('decides again for each call', 'if (sb.decided[layer] != frame + 1) {', 'if (1) {'),
    ('takes the layer from the wrong view bit', 'dview.setmask & 2', 'dview.setmask & 1'),
    ('takes any 3-layer depth image', 'd->width != d->height || ', ''),
    ('takes a color pass', 'if (st.rt.ncolor || ', 'if ('),
    ('takes images of any size', ' || d->width < 512', ''),
    ('takes images of any target', 'd->target != 4 || ', ''),
    ('takes images that are not arrays of 3', 'd->depth != 3 || ', ''),
    ('takes images without the render flag', '!(d->flags & 8) || ', ''),
    ('takes images that are not depth', ' ||\n        !renderer_format_is_depth(d->format)', ''),
    ('forgets the drawn frame notes', 'if (b->drawn_frame != frame) { b->prev_drawn = b->drawn_frame; b->drawn_frame = frame; }', ''),
    ('lets in layers the table lacks', 'layer >= SB_LAYERS', '0'),
    ('counts no skipped draw', 'stat_bump(is_draw ? &sb_skip_draws : &sb_skip_clears);', ''),
    ('counts no drawn layer', 'stat_bump(is_draw ? &sb_drawn[layer] : &sb_cleared[layer]);', ''),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exec', default=str(Path(__file__).resolve().parent.parent / 'runtime/nvn/vulkan/exec.c'))
    ap.add_argument('--self-check', action='store_true')
    a = ap.parse_args()
    src = Path(a.exec).read_text()
    with tempfile.TemporaryDirectory() as tmp:
        if not run(build(src), tmp, ENVS):
            return 1
        if a.self_check:
            bad = 0
            for name, old, new in MUTANTS:
                caught = not run(build(src, (old, new)), tmp, ENVS, quiet=True)
                print('%-40s %s' % (name, 'caught' if caught else 'MISSED'))
                bad += not caught
            return 1 if bad else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
