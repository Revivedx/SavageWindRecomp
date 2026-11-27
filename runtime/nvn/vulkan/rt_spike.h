// Ray tracing, phase 0: scene geometry measurement (rt_spike.c). Throwaway instrumentation.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { RT_SPIKE_DEPTH_WRITE = 1, RT_SPIKE_BLEND = 2, RT_SPIKE_DISCARD = 4, RT_SPIKE_VS_TEX = 8, RT_SPIKE_SKINNED = 16,
       RT_SPIKE_INSTANCED = 32, RT_SPIKE_VS_DRAWN_TEX = 64 };

typedef struct {
    uint8_t pass;    // 1 G-buffer (scene depth, 2+ color targets), 2 forward (scene, 1 target), 3 scene depth only, 4 shadow map
    uint8_t flags;   // RT_SPIKE_*
    uint8_t topo;    // NVN draw primitive
    uint32_t tris, inst;
    uint64_t geo;    // the mesh: per-vertex streams, index buffer, range, base vertex
    uint64_t ikey;   // ... without the streams (the shadow passes' shaders read fewer of them)
    uint64_t in_hash;  // the vertex shader's inputs besides the mesh: its constant buffers but the camera block, instanced streams
    uint64_t vs;
} RtSpikeDraw;

int rt_spike_period(void);  // 0: off
int rt_spike_slot(uint64_t frame);
RtSpikeDraw *rt_spike_draw(int slot);
bool rt_spike_geo_known(uint64_t key, uint32_t *verts);
void rt_spike_geo(uint64_t key, uint32_t tris, uint32_t verts, uint64_t frame);
void rt_spike_note(uint64_t fs_hash, const char *text);
