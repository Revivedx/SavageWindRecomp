// Descriptor sets that outlive their submission, and the constant data they point at (exec.c set_finish, stage_set).
//
// BOTW asks for ~8700 sets a frame in Kakariko. Sets of identical contents used to be shared only within one submission
// (~8 a frame), so ~1100 were still allocated and written every frame, and the submission's pools reset with it. Two
// thirds of those were the same as in one of the two frames before, except for three bindings whose contents rarely
// change but were copied into the transient ring each submission, at a new offset: the shader's compiler constants
// (binding 1), the texture scale table (18) and the tint (19). Those now live in a store addressed by their contents
// (vk_const_store), and sets live in pools that are reset by epochs, not with the submission slot (vk_set_get).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"

extern uint32_t vk_view_gen;  // textures.c
extern uint64_t vk_frame_counter(void);

static uint64_t hash_bytes(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001B3ull;
    return h ^ (h >> 32);
}

static bool env_off(const char *name, int *v) {
    if (*v < 0) *v = getenv(name) != NULL;
    return *v;
}

// ================================================================== constant store
// Two 2 MB halves of one buffer in GPU-only memory, filled in turn: when the current half is full the other one is
// emptied, once the GPU is done with everything that read it (by then, long ago). New contents go in through the
// submission's upload command buffer (rare once the game runs: none a second in Kakariko). In host-visible memory, as
// the transient ring, the GPU took ~3% longer per draw on a Radeon 660M. Contents are found by hash and compared against
// a CPU copy. A set pointing at a reused offset is still right: a descriptor is the address, and the same address is
// only handed out again for the contents now there.
// SAVAGEWIND_VK_NO_CONST_STORE=1: copied into the transient ring every submission, as before.
#define CS_HALF (2u << 20)
#define CS_TAB 1024
#define CS_MAX 0x4000  // (larger: transient ring)
static struct {
    VkBuffer buf;
    VmaAllocation mem;
    uint8_t *shadow;
    uint32_t used[2], gen[2];
    uint64_t last_use[2];
    int cur;
} cs;
typedef struct { uint64_t h; uint32_t off, size, klen, gen; } CsEnt;  // gen: of the half off is in
static CsEnt *cs_tab;
static uint64_t stat_cs_hit, stat_cs_new;

bool vk_const_store_on(void) {
    static int off = -1;
    return !env_off("SAVAGEWIND_VK_NO_CONST_STORE", &off);
}

static void cs_init(void) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2ull * CS_HALF,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &cs.buf, &cs.mem, NULL));
    cs.shadow = malloc(2ull * CS_HALF);
    cs_tab = calloc(CS_TAB, sizeof *cs_tab);
    cs.gen[0] = cs.gen[1] = 1;
}

static bool cs_valid(const CsEnt *e) { return e->h && e->gen == cs.gen[e->off >= CS_HALF]; }

// size bytes of data; the first klen of them tell contents apart (the rest is the same whenever those are: zeros).
VkDescriptorBufferInfo vk_const_store(const void *data, uint32_t klen, uint32_t size) {
    if (!cs.buf) cs_init();
    uint64_t pending = vk_pending_seq();
    uint64_t h = hash_bytes(size * 0xC2B2AE3D27D4EB4Full + klen, data, klen) | 1;
    uint32_t slot = (uint32_t)h & (CS_TAB - 1), put = slot;
    for (int probe = 0; probe < 8; probe++) {
        CsEnt *e = &cs_tab[(slot + probe) & (CS_TAB - 1)];
        if (!cs_valid(e)) { put = (slot + probe) & (CS_TAB - 1); break; }
        if (e->h == h && e->size == size && e->klen == klen && !memcmp(cs.shadow + e->off, data, klen)) {
            cs.last_use[e->off >= CS_HALF] = pending;
            stat_cs_hit++;
            return (VkDescriptorBufferInfo){cs.buf, e->off, size};
        }
    }
    uint32_t need = (size + 255) & ~255u;
    if (cs.used[cs.cur] + need > CS_HALF) {
        cs.cur ^= 1;
        vk_wait_seq(cs.last_use[cs.cur]);
        cs.gen[cs.cur]++;
        cs.used[cs.cur] = 0;
    }
    uint32_t off = (uint32_t)cs.cur * CS_HALF + cs.used[cs.cur];
    cs.used[cs.cur] += need;
    memcpy(cs.shadow + off, data, size);
    vkCmdUpdateBuffer(vk_upload_cmd(), cs.buf, off, (size + 3) & ~3u, cs.shadow + off);  // (the upload buffer runs first)
    cs_tab[put] = (CsEnt){h, off, size, klen, cs.gen[cs.cur]};
    cs.last_use[cs.cur] = pending;
    stat_cs_new++;
    return (VkDescriptorBufferInfo){cs.buf, off, size};
}

// Guest memory that does not change while the game runs (the compiler constants stored after a shader's code), by
// address: its contents are hashed once a frame, not for every draw.
bool vk_const_store_guest(uint64_t addr, uint32_t size, VkDescriptorBufferInfo *out) {
    if (size > CS_MAX) return false;
    static struct { uint64_t addr, frame; uint32_t size, gen; VkDescriptorBufferInfo d; } memo[256];
    __typeof__(memo[0]) *m = &memo[((addr >> 4) ^ (addr >> 14) ^ size) & 255];
    uint64_t frame = vk_frame_counter();
    if (cs.buf && m->addr == addr && m->size == size && m->frame == frame) {
        int half = m->d.offset >= CS_HALF;
        if (m->gen == cs.gen[half]) {
            cs.last_use[half] = vk_pending_seq();
            stat_cs_hit++;
            *out = m->d;
            return true;
        }
    }
    *out = vk_const_store((const void *)(uintptr_t)addr, size, size);
    m->addr = addr; m->size = size; m->frame = frame; m->d = *out; m->gen = cs.gen[out->offset >= CS_HALF];
    return true;
}

// ================================================================== sets
// Sets are kept by contents across submissions and frames, in the pools of the current epoch. When an epoch's pools are
// full, the next of three epochs is reset (the GPU finished with it two epochs ago) and every set is made again once.
// A view destroyed (vk_view_gen) or a heap re-import (exec.c setc_gen) drops them all: a new object may get the old
// handle. SAVAGEWIND_VK_NO_SET_KEEP=1: shared within one submission only, as before (exec.c).
#define EPOCHS 3
#define EPOCH_POOLS 4
#define SET_TAB 4096
#define LAST_TAB 256
typedef struct { VkDescriptorPool pool[EPOCH_POOLS]; int npools, cur; uint64_t last_use; uint32_t id; } Epoch;
static Epoch ep[EPOCHS];
static int ep_cur;
static uint32_t ep_ids;
typedef struct { uint64_t hash, hash2; VkDescriptorSet set; uint32_t epoch; } SetEnt;  // (layout, count and gen: hashed)
static SetEnt *set_tab, *last_tab;  // last_tab: the last set of each layout (most draws repeat the one before)
static uint64_t stat_alloc, stat_alloc_calls, stat_writes, stat_epochs;

bool vk_set_keep_on(void) {
    static int off = -1;
    return !env_off("SAVAGEWIND_VK_NO_SET_KEEP", &off);
}

static VkDescriptorPool pool_new(void) {
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLER, 4096},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32768}};
    VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                     .poolSizeCount = 6, .pPoolSizes = sizes};
    VkDescriptorPool p;
    VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &p));
    return p;
}

static void epoch_next(void) {
    ep_cur = (ep_cur + 1) % EPOCHS;
    Epoch *e = &ep[ep_cur];
    vk_wait_seq(e->last_use);
    for (int i = 0; i < e->npools; i++) vkResetDescriptorPool(g_vk.dev, e->pool[i], 0);
    e->cur = 0;
    e->id = ++ep_ids;
    stat_epochs++;
}

// Sets are allocated in batches per layout (SAVAGEWIND_VK_NO_SET_BATCH=1: one per call): a layout's batch grows with
// what it took in this epoch (1, 1, 2, 4 ... 32 a call), so at most about half of what a layout allocates goes unused
// (LAS Recomp d409a9ffdbfa). Spares die with their epoch.
#define SPARE_TAB 1024
#define SET_BATCH 32
typedef struct { VkDescriptorSetLayout layout; uint32_t epoch, n, total; VkDescriptorSet set[SET_BATCH]; } Spare;
static Spare *spares;

static bool pool_alloc(VkDescriptorSetLayout layout, uint32_t count, VkDescriptorSet *out) {
    VkDescriptorSetLayout ls[SET_BATCH];
    for (uint32_t i = 0; i < count; i++) ls[i] = layout;
    for (;;) {
        Epoch *e = &ep[ep_cur];
        if (e->cur == e->npools) {
            if (e->npools == EPOCH_POOLS) {
                if (count > 1) return false;  // (a new epoch drops the batch's layout from the cache: the caller asks for one)
                epoch_next();
                continue;
            }
            e->pool[e->npools++] = pool_new();
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = e->pool[e->cur], .descriptorSetCount = count, .pSetLayouts = ls};
        stat_alloc_calls++;
        if (vkAllocateDescriptorSets(g_vk.dev, &ai, out) == VK_SUCCESS) return true;
        e->cur++;
    }
}

static VkDescriptorSet set_alloc(VkDescriptorSetLayout layout) {
    static int nobatch = -1;
    stat_alloc++;
    VkDescriptorSet ds;
    if (env_off("SAVAGEWIND_VK_NO_SET_BATCH", &nobatch)) {
        pool_alloc(layout, 1, &ds);
        return ds;
    }
    if (!spares) spares = calloc(SPARE_TAB, sizeof *spares);
    uintptr_t k = (uintptr_t)layout;
    Spare *s = &spares[(k ^ (k >> 11) ^ (k >> 23)) & (SPARE_TAB - 1)];
    if (s->layout != layout || s->epoch != ep[ep_cur].id) { s->layout = layout; s->epoch = ep[ep_cur].id; s->n = s->total = 0; }
    if (s->n) return s->set[--s->n];
    uint32_t want = s->total < 1 ? 1 : s->total > SET_BATCH ? SET_BATCH : s->total;
    VkDescriptorSet got[SET_BATCH];
    if (want > 1 && pool_alloc(layout, want, got)) {
        s->total += want;
        for (uint32_t i = 1; i < want; i++) s->set[s->n++] = got[want - i];
        return got[0];
    }
    uint32_t id = ep[ep_cur].id;
    pool_alloc(layout, 1, &ds);
    if (ep[ep_cur].id != id) { s->epoch = ep[ep_cur].id; s->n = 0; s->total = 1; } else s->total++;
    return ds;
}

// Writes through an update template made once per layout and binding list (SAVAGEWIND_VK_NO_SET_TEMPLATE=1:
// vkUpdateDescriptorSets). The image and buffer infos are both 24 bytes: one array of either.
typedef union { VkDescriptorImageInfo i; VkDescriptorBufferInfo b; } DescInfo;
_Static_assert(sizeof(VkDescriptorImageInfo) == sizeof(VkDescriptorBufferInfo), "one stride for both");
#define TMPL_TAB 4096
typedef struct Tmpl { VkDescriptorSetLayout layout; uint64_t sig; uint32_t n; VkDescriptorUpdateTemplate t; struct Tmpl *next; } Tmpl;
static Tmpl *tmpl_tab[TMPL_TAB];

static VkDescriptorUpdateTemplate tmpl_get(VkDescriptorSetLayout layout, const VkWriteDescriptorSet *w, int n) {
    uint64_t sig = (uint64_t)(uintptr_t)layout;
    for (int i = 0; i < n; i++) sig = (sig ^ (w[i].dstBinding | (uint64_t)w[i].descriptorType << 32)) * 0x9E3779B97F4A7C15ull, sig ^= sig >> 29;
    Tmpl **head = &tmpl_tab[(sig ^ (sig >> 32)) & (TMPL_TAB - 1)];
    for (Tmpl *t = *head; t; t = t->next)
        if (t->layout == layout && t->sig == sig && t->n == (uint32_t)n) return t->t;
    VkDescriptorUpdateTemplateEntry ent[96];
    if (n > 96) return VK_NULL_HANDLE;
    for (int i = 0; i < n; i++)
        ent[i] = (VkDescriptorUpdateTemplateEntry){.dstBinding = w[i].dstBinding, .descriptorCount = 1, .descriptorType = w[i].descriptorType,
                                                   .offset = (size_t)i * sizeof(DescInfo), .stride = sizeof(DescInfo)};
    VkDescriptorUpdateTemplateCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
                                               .descriptorUpdateEntryCount = (uint32_t)n, .pDescriptorUpdateEntries = ent,
                                               .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
                                               .descriptorSetLayout = layout};
    Tmpl *t = calloc(1, sizeof *t);
    VK_CHECK(vkCreateDescriptorUpdateTemplate(g_vk.dev, &ci, NULL, &t->t));
    t->layout = layout; t->sig = sig; t->n = (uint32_t)n; t->next = *head;
    *head = t;
    return t->t;
}

static void set_write(VkDescriptorSet ds, VkDescriptorSetLayout layout, VkWriteDescriptorSet *w, int n) {
    static int notmpl = -1;
    stat_writes += (uint64_t)n;
    if (!n) return;
    VkDescriptorUpdateTemplate t = env_off("SAVAGEWIND_VK_NO_SET_TEMPLATE", &notmpl) ? VK_NULL_HANDLE : tmpl_get(layout, w, n);
    if (t) {
        DescInfo d[96];
        for (int i = 0; i < n; i++)
            if (w[i].pImageInfo) d[i].i = *w[i].pImageInfo;
            else d[i].b = *w[i].pBufferInfo;
        vkUpdateDescriptorSetWithTemplate(g_vk.dev, ds, t, d);
        return;
    }
    for (int i = 0; i < n; i++) w[i].dstSet = ds;
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
}

// The set holding these writes (image or buffer infos; dynamic buffers at offset 0). gen: exec.c's setc_gen. *shared:
// found, not made.
VkDescriptorSet vk_set_get(VkDescriptorSetLayout layout, VkWriteDescriptorSet *w, int n, uint64_t gen, bool *shared) {
    if (!set_tab) { set_tab = calloc(SET_TAB, sizeof *set_tab); last_tab = calloc(LAST_TAB, sizeof *last_tab); }
    uint64_t h = (uint64_t)(uintptr_t)layout ^ (gen << 32 | vk_view_gen) * 0x9E3779B97F4A7C15ull ^ (uint64_t)n << 56;
    uint64_t h2 = h ^ 0x6A09E667F3BCC909ull;
    for (int i = 0; i < n; i++) {
        uint64_t v[4];
        v[0] = w[i].dstBinding | (uint64_t)w[i].descriptorType << 32;
        if (w[i].pImageInfo) {
            v[1] = (uint64_t)(uintptr_t)w[i].pImageInfo->sampler;
            v[2] = (uint64_t)(uintptr_t)w[i].pImageInfo->imageView;
            v[3] = w[i].pImageInfo->imageLayout;
        } else {
            v[1] = (uint64_t)(uintptr_t)w[i].pBufferInfo->buffer;
            v[2] = w[i].pBufferInfo->offset;
            v[3] = w[i].pBufferInfo->range;
        }
        for (int k = 0; k < 4; k++) {
            h = (h ^ v[k]) * 0x9E3779B97F4A7C15ull; h ^= h >> 29;
            h2 = (h2 ^ v[k]) * 0xC2B2AE3D27D4EB4Full; h2 ^= h2 >> 31;
        }
    }
    h ^= h >> 32;
    Epoch *cur = &ep[ep_cur];
    uintptr_t lk = (uintptr_t)layout;
    SetEnt *last = &last_tab[(lk ^ (lk >> 9) ^ (lk >> 17)) & (LAST_TAB - 1)];
    SetEnt *e = &set_tab[(h ^ (h >> 31)) & (SET_TAB - 1)];
    for (int pass = 0; pass < 2; pass++) {
        SetEnt *c = pass ? e : last;
        if (c->epoch == cur->id && c->hash == h && c->hash2 == h2) {
            cur->last_use = vk_pending_seq();
            *last = *c;
            *shared = true;
            return c->set;
        }
    }
    *shared = false;
    VkDescriptorSet ds = set_alloc(layout);  // (may start a new epoch)
    set_write(ds, layout, w, n);
    ep[ep_cur].last_use = vk_pending_seq();
    *e = (SetEnt){h, h2, ds, ep[ep_cur].id};
    *last = *e;
    return ds;
}

// "allocated A in C calls, W descriptors written, E new epochs, constants H found N new" since the last call.
int vk_set_stats(char *buf, size_t size) {
    int r = snprintf(buf, size, "set alloc %llu in %llu calls, %llu written, %llu epochs, const %llu found %llu new",
                     (unsigned long long)stat_alloc, (unsigned long long)stat_alloc_calls, (unsigned long long)stat_writes,
                     (unsigned long long)stat_epochs, (unsigned long long)stat_cs_hit, (unsigned long long)stat_cs_new);
    stat_alloc = stat_alloc_calls = stat_writes = stat_epochs = stat_cs_hit = stat_cs_new = 0;
    return r;
}
