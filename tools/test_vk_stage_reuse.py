#!/usr/bin/env python3
"""Differential checks for the actual fragment-preparation reuse functions."""
import os
from pathlib import Path
import subprocess
import tempfile


def function(source, name):
    pos=source.index(name+'(')
    # Skip forward declarations and calls: the definition is the one whose parameter list is followed by a brace.
    while True:
        end_sig=min(i for i in (source.find(';',pos),source.find('{',pos)) if i>=0)
        if source[end_sig]=='{' and source.rfind('static ',0,pos)>source.rfind('\n\n',0,pos):break
        pos=source.index(name+'(',pos+1)
    start=source.rfind('static ',0,pos)
    brace=source.index('{',pos)
    end,depth=brace+1,1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define MAX_BIND 96
#define GMEM_BUFFER_INDEX 20
#define GMEM2_BUFFER_INDEX 21
#define MVEC_HISTORY_BINDING 22
#define GSNAP_BUFFER_INDEX 23
#define DEDUPE_RSCALE_F 0
#define DEDUPE_RSCALE_V 1
#define DEDUPE_TINT 2
#define DEDUPE_DRV_C 3
#define DEDUPE_DRV_F 4
#define DEDUPE_DRV_V 5
#define SETC_TAB 4096
typedef struct {uint64_t addr, size;} Range;
typedef struct {uint32_t set, binding; VkDescriptorType type;} VkBindingInfo;
typedef struct {
    VkBindingInfo binds[MAX_BIND]; int nbinds;
    uint8_t bind_order[2][MAX_BIND], nstage[2], nlate[2];
    bool reuse_stage[2]; uint32_t reuse_mask[2];
    uint8_t set_idx[2][MAX_BIND]; uint8_t set_n[2];
    bool has_rscale[2], has_tint[2];
} Prog;
typedef struct {int ntex; uint32_t cbuf_size[18]; bool uses_gmem, writes_gmem;} ShaderInfo;
typedef struct {VkBuffer buf; uint64_t off; void *ptr;} VkSpan;
typedef struct {uint64_t code_addr; uint32_t const_offset, const_size;} NvnShaderStage;
typedef struct {NvnShaderStage stages[6];} NvnProgram;
static struct {Range ubuf[6][18], sbuf[6][16]; NvnProgram *program;} st;
static uint64_t gcopy_gen=1, uub_serial, gw_gen;
static struct {float s;} sdw_fix;
static struct {float sx;} up_fix;
static bool fxaa_off;
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];
#define LOG(...) ((void)0)
static _Atomic int stage_cpu_options=-1;
#define CPU_OPTS_REUSE 3
#define CPU_OPTS_VALID (CPU_OPTS_REUSE | 3824)
#define CPU_OPTS_DEFAULT 3312
static bool g_opt_dispatch_fast, g_opt_cb_unlink;
static void stage_cpu_publish(int mask) {g_opt_dispatch_fast=(mask&1024)!=0;g_opt_cb_unlink=(mask&2048)!=0;}
static _Atomic uint64_t stage_reuse_calls, stage_reuse_eligible, stage_reuse_hits;
typedef struct {bool failed; int mask;} ControlReply;
static void control_fail(ControlReply *r,const char *text) {(void)text;r->failed=true;}
static void control_field_int(ControlReply *r,const char *key,int mask) {if(!strcmp(key,"mask"))r->mask=mask;}
static bool maxwell_gmem_bda;
#define HEAP_BASE 0x7240000000ull
static uint64_t gmem_size;  // (0: guest memory not mapped as one sparse buffer)
static bool gmem_sparse;
static bool plat_guest_commit_sparse_range(uint64_t addr, uint64_t size) {(void)addr;(void)size;return true;}
#define FATAL(...) abort()
static uint64_t stat_sets, stat_sets_reused, setc_gen, pending_seq=1;
typedef struct {uint64_t seq,hash,hash2; VkDescriptorSetLayout layout; VkDescriptorSet set; uint32_t n;} SetEnt;
static SetEnt *setc;
static uint64_t captured[MAX_BIND][4];
static int ncaptured, allocations, calls, last_copy_size;
static unsigned char last_copy[4096];
static uint64_t vk_pending_seq(void) {return pending_seq;}
static VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {(void)layout; return (VkDescriptorSet)(uintptr_t)++allocations;}
static void rec_UpdateDescriptorSets(uint32_t n, const VkWriteDescriptorSet *w) {
    ncaptured=n;
    for(unsigned i=0;i<n;i++) {
        assert(w[i].sType==VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET && !w[i].pNext);
        assert(w[i].descriptorCount==1 && w[i].dstArrayElement==0 && !w[i].pTexelBufferView);
        captured[i][0]=w[i].dstBinding | (uint64_t)w[i].descriptorType<<32;
        if(w[i].pImageInfo) {
            assert(!w[i].pBufferInfo);
            captured[i][1]=(uintptr_t)w[i].pImageInfo->sampler;
            captured[i][2]=(uintptr_t)w[i].pImageInfo->imageView;
            captured[i][3]=w[i].pImageInfo->imageLayout;
        } else {
            captured[i][1]=(uintptr_t)w[i].pBufferInfo->buffer;
            captured[i][2]=w[i].pBufferInfo->offset;
            captured[i][3]=w[i].pBufferInfo->range;
        }
    }
}
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t x,uint32_t y,uint32_t z) {
    (void)si; calls++; uint64_t a=k>=3 ? st.ubuf[stage][k-3].addr : k==1 ? st.program->stages[stage].code_addr : 0; return (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)(100+stage), (uint64_t)(k*256+x+y+z+a), 256};
}
static VkDescriptorBufferInfo gmem_info(void) {calls++; return (VkDescriptorBufferInfo){(VkBuffer)51,256,512};}
static VkDescriptorBufferInfo gmem_info_hi(void) {calls++; return (VkDescriptorBufferInfo){(VkBuffer)52,512,1024};}
static VkDescriptorBufferInfo mvec_history_info(void) {calls++; return (VkDescriptorBufferInfo){(VkBuffer)53,768,1536};}
static VkDescriptorBufferInfo gsnap_info(int s) {calls++; return (VkDescriptorBufferInfo){(VkBuffer)54,(uint64_t)s*256,2048};}
static VkSampler vk_default_sampler(bool b) {(void)b;return (VkSampler)71;}
static VkImageView vk_dummy_view(int t) {(void)t;return (VkImageView)72;}
#define TEXTYPE_2D 1
static void texture_info(int stage, ShaderInfo *si,int t,VkDescriptorImageInfo *ii,float sc[2],uint32_t wh[2]) {
    (void)si; calls++;
    *ii=(VkDescriptorImageInfo){(VkSampler)(uintptr_t)(100+t),(VkImageView)(uintptr_t)(200+t+stage),VK_IMAGE_LAYOUT_GENERAL};
    sc[0]=1+t;sc[1]=2+t;wh[0]=128+t;wh[1]=64+t;
}
static VkSpan copy_dedupe_in(int id,const void *p,uint64_t n,uint64_t total,uint64_t align) {
    (void)align; (void)total; calls++; assert(n<=sizeof last_copy);
    memcpy(last_copy,p,n); last_copy_size=n;
    uint64_t h=id+1; for(uint64_t i=0;i<n;i++)h=h*33+((const unsigned char*)p)[i];
    return (VkSpan){(VkBuffer)(uintptr_t)(1000+id),h&~255ull,NULL};
}
static VkSpan copy_dedupe(int id,const void *p,uint64_t n,uint64_t align) {return copy_dedupe_in(id,p,n,n,align);}
static VkDescriptorBufferInfo ubo_span(VkSpan s,uint64_t n) {return (VkDescriptorBufferInfo){s.buf,s.off,n};}
// (the identity render-scale row and the zero tint: transient memory of their own, made once a submission)
static unsigned char transient_mem[1<<16];
static uint64_t transient_used;
static VkSpan vk_transient(uint64_t size,uint64_t align) {
    uint64_t off=(transient_used+align-1)&~(align-1);
    if(off+size>sizeof transient_mem)off=0;
    transient_used=off+size;
    return (VkSpan){(VkBuffer)(uintptr_t)900,off,transient_mem+off};
}
static VkSpan vk_transient_copy(const void *p,uint64_t size,uint64_t align) {
    VkSpan s=vk_transient(size,align);memcpy(s.ptr,p,size);return s;
}
'''

MAIN = r'''
static void call(Prog *p, ShaderInfo *si, VkDescriptorImageInfo *images, float *row, float *tint, uint32_t *d, int *n) {
    *n=0;stage_set(p,(VkDescriptorSetLayout)5,1,1,si,row,tint,0,0,0,images,d,n);
}
int main(void) {
    NvnProgram program={0};st.program=&program;
    Prog p={.nbinds=4,.binds={{1,3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
        {1,32,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},{1,18,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
        {1,19,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}}};
    prog_index_sets(&p);
    stage_reuse_plan(&p);
    ShaderInfo si={.ntex=1};
    VkDescriptorImageInfo images[1]={{(VkSampler)5,(VkImageView)6,VK_IMAGE_LAYOUT_GENERAL}};
    float row[4]={1,1},tint[8]={0};uint32_t da[32],db[32];int na,nb;
    atomic_store(&stage_cpu_options,2);
    bool cached=getenv("SAVAGEWIND_VK_NO_SET_CACHE")==NULL;
    // Same material and dynamic offsets, even when paired with another vertex program.
    call(&p,&si,images,row,tint,da,&na);int before=calls;
    call(&p,&si,images,row,tint,db,&nb);
    assert(na==nb && !memcmp(da,db,na*4));assert(cached ? calls==before : calls>before);
    Prog p2=p;NvnProgram program2=program;st.program=&program2;
    before=calls;call(&p2,&si,images,row,tint,db,&nb);
    assert(cached ? calls==before : calls>before);
    // An unused binding can change without changing this fragment stage.
    st.ubuf[1][8].addr++;before=calls;call(&p2,&si,images,row,tint,db,&nb);
    assert(cached ? calls==before : calls>before);
    for(int change=0;change<15;change++) {
        switch(change) {
        case 0:st.ubuf[1][0].addr+=256;break;case 1:st.ubuf[1][0].size++;break;
        case 2:gcopy_gen++;break;case 3:uub_serial++;break;case 4:gw_gen++;break;
        case 5:pending_seq++;break;case 6:setc_gen++;break;
        case 7:images[0].imageView=(VkImageView)7;break;case 8:images[0].sampler=(VkSampler)8;break;
        case 9:images[0].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;break;
        case 10:row[0]++;break;case 11:tint[0]++;break;case 12:tex_sc[1][0][0]++;break;
        case 13:si.ntex=0;break;case 14:p2.reuse_mask[1]|=1;st.sbuf[1][0].addr++;break;
        }
        before=calls;call(&p2,&si,images,row,tint,db,&nb);assert(calls>before);
        before=calls;call(&p2,&si,images,row,tint,db,&nb);assert(cached ? calls==before : calls>before);
    }
    // Compiler constants depend on the fragment code blob, not just the translated shader.
    p2.reuse_mask[1]|=2;call(&p2,&si,images,row,tint,db,&nb);
    program2.stages[1].code_addr++;before=calls;call(&p2,&si,images,row,tint,db,&nb);assert(calls>before);
    program2.stages[1].const_offset++;before=calls;call(&p2,&si,images,row,tint,db,&nb);assert(calls>before);
    program2.stages[1].const_size++;before=calls;call(&p2,&si,images,row,tint,db,&nb);assert(calls>before);
    // The conservative reference mode must invalidate for a different program.
    atomic_store(&stage_cpu_options,1);call(&p2,&si,images,row,tint,db,&nb);
    st.program=&program;before=calls;call(&p2,&si,images,row,tint,db,&nb);assert(calls>before);
    st.program=&program2;atomic_store(&stage_cpu_options,2);
    // Match the unmodified path's descriptors and dynamic offsets for changing resources.
    for(int trial=0;trial<2000;trial++) {
        st.ubuf[1][0].addr=(uint64_t)trial*256;uub_serial++;
        uint64_t expected[MAX_BIND][4];
        stage_set_prepare(&p2,(VkDescriptorSetLayout)5,1,1,&si,row,tint,0,0,0,images,da,&(int){0});
        int n=ncaptured;memcpy(expected,captured,sizeof expected);
        call(&p2,&si,images,row,tint,db,&nb);
        assert(n==ncaptured && !memcmp(expected,captured,sizeof expected));
        assert(nb==1 && db[0]==768+trial*256);
    }
    for(int special=0;special<6;special++) {
        if(special==0)sdw_fix.s=2;if(special==1)up_fix.sx=2;if(special==2)fxaa_off=true;
        if(special==3)si.uses_gmem=true;if(special==4)si.writes_gmem=true;if(special==5)p2.reuse_stage[1]=false;
        before=calls;call(&p2,&si,images,row,tint,db,&nb);assert(calls>before);
        sdw_fix.s=up_fix.sx=0;fxaa_off=si.uses_gmem=si.writes_gmem=false;p2.reuse_stage[1]=true;
    }
    free(setc);puts("PASS: material reuse, cross-program sharing, used-binding invalidation and 2000 differential cases");
}
'''

def main():
    root=Path(__file__).resolve().parent.parent
    source=(root/'runtime/nvn/vulkan/exec.c').read_text()
    c='\n'.join([FIXTURE,function(source,'prog_index_sets'),function(source,'stage_reuse_plan'),function(source,'stage_cpu_opts_init'),function(source,'stage_cpu_opts'),
                  function(source,'stage_reuse_mode'),function(source,'mum64'),function(source,'set_finish'),
                  source[source.index('typedef struct {\n    VkWriteDescriptorSet w[MAX_BIND];'):source.index('// STAGE_REUSE_TEST_BEGIN')].replace('stage_set(', 'stage_set_prepare('),
                  source[source.index('// STAGE_REUSE_TEST_BEGIN'):source.index('// STAGE_REUSE_TEST_END')].replace('stage_set(', 'stage_set_prepare(').replace('stage_set_reuse(', 'stage_set('),MAIN])
    with tempfile.TemporaryDirectory(prefix='swr-reuse-') as d:
        path=Path(d)/'test.c';path.write_text(c);exe=Path(d)/'test'
        subprocess.run([os.environ.get('CC','clang'),'-O1','-g','-fsanitize=address,undefined',str(path),'-o',str(exe)],check=True)
        for off in (False,True):
            env=dict(os.environ);env.pop('SAVAGEWIND_VK_NO_SET_CACHE',None)
            if off:env['SAVAGEWIND_VK_NO_SET_CACHE']='1'
            subprocess.run([str(exe)],env=env,check=True)

if __name__=='__main__':main()
