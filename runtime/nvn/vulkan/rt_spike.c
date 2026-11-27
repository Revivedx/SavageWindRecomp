// Ray tracing, phase 0 (docs/research/ray-tracing.md): what an acceleration structure of BOTW's scene would hold.
// Measurement only, SAVAGEWIND_RT_SPIKE=<frames between samples> (1: 240). Every period, two consecutive presented
// frames' scene and shadow draws are recorded (exec.c rt_spike_hook) and compared: how much geometry there is, how much
// of it is the same mesh with the same vertex shader inputs as a frame before (a bottom-level structure built once),
// and how much changes (skinned, instanced, displaced by a texture). Throwaway instrumentation: not for main.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "rt_spike.h"

#define MAXD 32768
static RtSpikeDraw fr[2][MAXD];
static uint32_t nfr[2];
static uint64_t fr_frame[2] = {~0ull, ~0ull};
static int period = -1;

int rt_spike_period(void) {
    if (period < 0) {
        const char *e = getenv("SAVAGEWIND_RT_SPIKE");
        period = e ? atoi(e) : 0;
        if (period == 1) period = 240;
    }
    return period;
}

// The meshes seen so far (any pass): geometry key -> triangles and vertices, what a cache of structures would hold
#define GEO_CAP (1u << 19)
static struct { uint64_t key; uint32_t tris, verts; uint64_t last; } *geo;
static uint32_t geo_n;
static uint64_t geo_tris, geo_verts;

static uint32_t geo_slot(uint64_t key) {
    if (!geo) geo = calloc(GEO_CAP, sizeof *geo);
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 45) & (GEO_CAP - 1);
    while (geo[i].key && geo[i].key != key) i = (i + 1) & (GEO_CAP - 1);
    return i;
}

bool rt_spike_geo_known(uint64_t key, uint32_t *verts) {
    uint32_t i = geo_slot(key ? key : 1);
    if (!geo[i].key) return false;
    *verts = geo[i].verts;
    return true;
}

void rt_spike_geo(uint64_t key, uint32_t tris, uint32_t verts, uint64_t frame) {
    uint32_t i = geo_slot(key ? key : 1);
    if (!geo[i].key && geo_n < GEO_CAP / 2) {
        geo[i].key = key ? key : 1; geo[i].tris = tris; geo[i].verts = verts;
        geo_n++; geo_tris += tris; geo_verts += verts;
    }
    geo[i].last = frame;
}

// Small sets for the report: (key -> count)
typedef struct { uint64_t key, n, t; } Cnt;
static int cnt_add(Cnt *c, int *n, int cap, uint64_t key, uint64_t t) {
    for (int i = 0; i < *n; i++) if (c[i].key == key) { c[i].n++; c[i].t += t; return i; }
    if (*n < cap) { c[*n] = (Cnt){key, 1, t}; return (*n)++; }
    return -1;
}
static int cnt_cmp(const void *a, const void *b) {
    const Cnt *x = a, *y = b;
    return x->t < y->t ? 1 : x->t > y->t ? -1 : 0;
}

#define NOTE_CAP 96
static struct { uint64_t fs; char text[200]; uint32_t n; } notes[NOTE_CAP];
static int nnotes;
// A fact about a fragment shader, logged once per report (exec.c: the cascades' readers, the G-buffer's readers)
void rt_spike_note(uint64_t fs_hash, const char *text) {
    for (int i = 0; i < nnotes; i++) if (notes[i].fs == fs_hash && !strcmp(notes[i].text, text)) { notes[i].n++; return; }
    if (nnotes >= NOTE_CAP) return;
    notes[nnotes].fs = fs_hash; notes[nnotes].n = 1;
    snprintf(notes[nnotes].text, sizeof notes[nnotes].text, "%s", text);
    nnotes++;
}

static int u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static bool has(const uint64_t *s, uint32_t n, uint64_t k) { return bsearch(&k, s, n, 8, u64_cmp) != NULL; }
static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static const char *pass_name[] = {"other", "gbuffer", "forward", "depth-only", "shadow"};

static void report(void) {
    const RtSpikeDraw *a = fr[0], *b = fr[1];
    uint32_t na = nfr[0], nb = nfr[1];
    // frame A's (geometry, inputs) pairs and its geometries, by pass group (scene: 1-3, shadow: 4)
    static uint64_t a_in[MAXD], a_geo[MAXD], b_scene_i[MAXD], b_scene_geo[MAXD];
    uint32_t nb_scene = 0;
    for (uint32_t i = 0; i < na; i++) {
        a_in[i] = mix(mix(a[i].geo, a[i].in_hash), a[i].pass == 4);
        a_geo[i] = mix(a[i].geo, a[i].pass == 4);
    }
    qsort(a_in, na, 8, u64_cmp);
    qsort(a_geo, na, 8, u64_cmp);
    for (uint32_t i = 0; i < nb; i++) if (b[i].pass >= 1 && b[i].pass <= 3) { b_scene_i[nb_scene] = b[i].ikey; b_scene_geo[nb_scene++] = b[i].geo; }
    qsort(b_scene_i, nb_scene, 8, u64_cmp);
    qsort(b_scene_geo, nb_scene, 8, u64_cmp);

    LOG("[rtspike] frames %llu/%llu: draws %u -> %u; meshes seen so far %u (%.2f M triangles, %.2f M vertices)\n",
        (unsigned long long)fr_frame[0], (unsigned long long)fr_frame[1], na, nb, geo_n, geo_tris / 1e6, geo_verts / 1e6);
    for (int p = 1; p <= 4; p++) {
        // classes: 0 same inputs as the frame before, 1 skinned, 2 instanced, 3 vertex shader samples a drawn texture,
        // 4 other change with the mesh in the frame before (moved), 5 mesh not in the frame before (new)
        uint64_t cd[6] = {0}, ct[6] = {0}, draws = 0, tris = 0, inst_tris = 0, verts = 0;
        uint64_t f_d[4] = {0}, f_t[4] = {0};  // discard, blend, no depth write, vs textures
        uint64_t off_d = 0, off_t = 0, off_ig = 0;  // shadow: meshes not in the scene passes of the frame
        uint64_t topo[8] = {0};
        static uint64_t ug[MAXD];
        uint32_t nug = 0;
        static Cnt vs_changed[256];
        int nvc = 0;
        for (uint32_t i = 0; i < nb; i++) {
            const RtSpikeDraw *d = &b[i];
            if (d->pass != p) continue;
            uint64_t t = (uint64_t)d->tris * d->inst;
            draws++; tris += t; inst_tris += d->inst > 1 ? t : 0;
            topo[d->topo < 8 ? d->topo : 7]++;
            ug[nug++] = d->geo;
            int c;
            if (has(a_in, na, mix(mix(d->geo, d->in_hash), p == 4))) c = 0;
            else if (d->flags & RT_SPIKE_SKINNED) c = 1;
            else if (d->flags & RT_SPIKE_INSTANCED) c = 2;
            else if (d->flags & RT_SPIKE_VS_DRAWN_TEX) c = 3;
            else c = has(a_geo, na, mix(d->geo, p == 4)) ? 4 : 5;
            cd[c]++; ct[c] += t;
            if (c == 4) cnt_add(vs_changed, &nvc, 256, d->vs, t);
            if (d->flags & RT_SPIKE_DISCARD) { f_d[0]++; f_t[0] += t; }
            if (d->flags & RT_SPIKE_BLEND) { f_d[1]++; f_t[1] += t; }
            if (!(d->flags & RT_SPIKE_DEPTH_WRITE)) { f_d[2]++; f_t[2] += t; }
            if (d->flags & RT_SPIKE_VS_TEX) { f_d[3]++; f_t[3] += t; }
            if (p == 4 && !has(b_scene_i, nb_scene, d->ikey)) { off_d++; off_t += t; }
        }
        if (!draws) continue;
        qsort(ug, nug, 8, u64_cmp);
        uint32_t uniq = 0;
        uint64_t utris = 0;
        for (uint32_t i = 0; i < nug; i++) {
            if (i && ug[i] == ug[i - 1]) continue;
            uniq++;
            uint32_t s = geo_slot(ug[i] ? ug[i] : 1);
            utris += geo[s].tris; verts += geo[s].verts;
            if (p == 4 && !has(b_scene_geo, nb_scene, ug[i])) off_ig++;
        }
        LOG("[rtspike]  %-10s draws %llu, triangles %.3f M (instanced copies %.3f M); meshes %u: %.3f M triangles, %.3f M vertices\n",
            pass_name[p], (unsigned long long)draws, tris / 1e6, inst_tris / 1e6, uniq, utris / 1e6, verts / 1e6);
        LOG("[rtspike]    same inputs %llu (%.3f M) | skinned %llu (%.3f M) | instanced %llu (%.3f M) | vs drawn texture %llu (%.3f M) | moved %llu (%.3f M) | new %llu (%.3f M)\n",
            (unsigned long long)cd[0], ct[0] / 1e6, (unsigned long long)cd[1], ct[1] / 1e6, (unsigned long long)cd[2], ct[2] / 1e6,
            (unsigned long long)cd[3], ct[3] / 1e6, (unsigned long long)cd[4], ct[4] / 1e6, (unsigned long long)cd[5], ct[5] / 1e6);
        LOG("[rtspike]    discard %llu (%.3f M) | blend %llu (%.3f M) | no depth write %llu (%.3f M) | vs textures %llu (%.3f M) | modes 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu other %llu\n",
            (unsigned long long)f_d[0], f_t[0] / 1e6, (unsigned long long)f_d[1], f_t[1] / 1e6, (unsigned long long)f_d[2], f_t[2] / 1e6,
            (unsigned long long)f_d[3], f_t[3] / 1e6, (unsigned long long)topo[0], (unsigned long long)topo[1], (unsigned long long)topo[2],
            (unsigned long long)topo[3], (unsigned long long)topo[4], (unsigned long long)(topo[5] + topo[6] + topo[7]));
        if (p == 4)
            LOG("[rtspike]    index ranges not drawn by the scene passes: %llu draws (%.3f M triangles); meshes (with streams) not in the scene: %llu of %u\n",
                (unsigned long long)off_d, off_t / 1e6, (unsigned long long)off_ig, uniq);
        if (nvc) {
            qsort(vs_changed, (size_t)nvc, sizeof(Cnt), cnt_cmp);
            char buf[400];
            int o = 0;
            for (int i = 0; i < nvc && i < 6; i++)
                o += snprintf(buf + o, sizeof buf - (size_t)o, " vs %llx x%llu (%.0f K)", (unsigned long long)vs_changed[i].key,
                              (unsigned long long)vs_changed[i].n, vs_changed[i].t / 1e3);
            LOG("[rtspike]    moved:%s\n", buf);
        }
    }
    for (int i = 0; i < nnotes; i++) LOG("[rtspike]  note fs %016llx x%u: %s\n", (unsigned long long)notes[i].fs, notes[i].n, notes[i].text);
    nnotes = 0;
}

// The frame a draw belongs to: 0 / 1 the sample pair's first / second frame, -1 not sampled
int rt_spike_slot(uint64_t frame) {
    int p = rt_spike_period();
    if (!p) return -1;
    if (fr_frame[1] != ~0ull && frame > fr_frame[1]) {  // (the pair is complete)
        report();
        fr_frame[0] = fr_frame[1] = ~0ull;
        nfr[0] = nfr[1] = 0;
    }
    if (frame % (uint64_t)p == 0) {
        if (fr_frame[0] != frame) { fr_frame[0] = frame; nfr[0] = 0; fr_frame[1] = ~0ull; nfr[1] = 0; }
        return 0;
    }
    if (fr_frame[0] != ~0ull && frame == fr_frame[0] + 1) { fr_frame[1] = frame; return 1; }
    if (fr_frame[0] != ~0ull && fr_frame[1] == ~0ull && frame > fr_frame[0] + 1) { fr_frame[0] = ~0ull; nfr[0] = 0; }
    return -1;
}

RtSpikeDraw *rt_spike_draw(int slot) {
    if (slot < 0 || nfr[slot] >= MAXD) return NULL;
    return &fr[slot][nfr[slot]++];
}
