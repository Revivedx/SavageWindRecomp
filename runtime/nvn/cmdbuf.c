// NVN command buffers: commands are recorded as CmdRec records in host memory; the guest-provided command
// memory still drives the game's own memory management and out-of-memory callbacks (see below).
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"

#define JUMP_RESERVE (sizeof(CmdRec) + 8)

// What the game sees and where the records go are kept apart. The game sizes command memory for the GPU commands
// real NVN writes, and the records here are larger: BOTW 1.8.2 measures each display list by recording it once
// ("calc resource"), then allocates exactly that much from the model's heap, which the resource size table sized for
// real NVN; with record sizes the heap runs out and the lists get no memory pool.
// - Guest side: command memory size, used and free, and the out-of-memory callback, count cmd_cost bytes per
//   command, an estimate of what NVN writes for it (kept at or under the real size: memory the game allocates from
//   an estimate that is too small is only accounting, the records do not live there).
// - Host side: records go to shadow storage keyed by the guest memory they are accounted to: the chunk while it is
//   being recorded into, then the part of it that was used. When the game writes commands over that memory again,
//   what was recorded there is dead (on hardware it is overwritten): the lists the first bytes go over at once, the
//   others as the recording reaches them. The memory the game adds is often far more than it records there (BOTW
//   gives each deferred lighting composite's material list a 0x200-byte window, records the 0x18 bytes it measured
//   and packs the next list right after); killing the whole window lost the lists packed there before, which the
//   frame still calls (the composites' material ID, and with it the lighting). Dead blocks are reused three frames later,
//   not at once: the command buffer that recorded there may still chain on from them. Memory added without a guest
//   chunk (a null pool) gets shadow storage of its own.
// - Lists in a memory pool die with it (nvnMemoryPoolFinalize: cmdbuf_pool_gone): on hardware their commands were in
//   that memory. Without this, every list of every model ever unloaded stayed (issue #74: +6-10k shadows a load).
// - A shadow's storage starts small and grows a block at a time: the memory the game adds is often far more than it
//   records there (BOTW adds hundreds of KB where it records a few hundred bytes).
// A command handle is the host address of a list's first record.
static uint32_t cmd_cost(uint32_t op, uint32_t extra) { (void)op; return 8 + ((extra + 3) & ~3u); }

typedef struct { uint8_t *mem; uint64_t size; uint64_t frame; } HBlock;
typedef struct Shadow {
    uint64_t lo, hi;  // the guest memory it stands for (written so far); lo == hi == 0: none
    uint64_t win;     // end of the memory the game added there (0: none)
    uint64_t free_to; // claims below this go over no other live shadow (shadow_claim takes no lock)
    HBlock *blocks;
    int nblocks, cap;
    int users;        // command buffers recording into it
    bool dead;        // overlapped: out of the live list (freed when its last user leaves)
} Shadow;

static Shadow **g_live;  // shadows of guest memory, sorted by lo, never overlapping
static int g_nlive, g_caplive;
static HBlock *g_free;   // blocks ready for reuse
static int g_nfree, g_capfree;
static uint64_t g_free_bytes;
#define FREE_BUDGET (64ull << 20)  // dead blocks past this go back to the C heap
static HBlock *g_grave;  // dead blocks, oldest first, until three frames have passed
static int g_ngrave, g_capgrave;
static pthread_mutex_t g_shadow_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t renderer_present_count(void);

static void push_block(HBlock **v, int *n, int *cap, HBlock b) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        *v = realloc(*v, *cap * sizeof **v);
    }
    (*v)[(*n)++] = b;
}

static void block_release(HBlock b) {  // (lock held)
    b.frame = renderer_present_count();
    push_block(&g_grave, &g_ngrave, &g_capgrave, b);
}

static HBlock block_get(uint64_t want) {  // (lock held) a free block that fits without wasting much, or a new one
    uint64_t now = renderer_present_count();
    static int stats = -1;
    static uint64_t stats_at;
    if (stats < 0) stats = getenv("SAVAGEWIND_CMD_STATS") != NULL;
    if (stats && now >= stats_at) {  // (test) what the command records hold, every 600 frames
        uint64_t live = 0;
        for (int i = 0; i < g_nlive; i++)
            for (int j = 0; j < g_live[i]->nblocks; j++) live += g_live[i]->blocks[j].size;
        LOG("[cmdbuf] frame %llu: %d lists' shadows, %llu MB of records; %d free blocks, %llu MB\n",
            (unsigned long long)now, g_nlive, (unsigned long long)(live >> 20), g_nfree,
            (unsigned long long)(g_free_bytes >> 20));
        stats_at = now + 600;
    }
    int k = 0;
    for (; k < g_ngrave && now - g_grave[k].frame >= 3; k++) {
        if (g_free_bytes + g_grave[k].size > FREE_BUDGET) { free(g_grave[k].mem); continue; }
        g_free_bytes += g_grave[k].size;
        push_block(&g_free, &g_nfree, &g_capfree, g_grave[k]);
    }
    if (k) memmove(g_grave, g_grave + k, (g_ngrave -= k) * sizeof *g_grave);
    for (int i = g_nfree - 1; i >= 0; i--)
        if (g_free[i].size >= want && g_free[i].size <= want * 4) {
            HBlock b = g_free[i];
            g_free[i] = g_free[--g_nfree];
            g_free_bytes -= b.size;
            return b;
        }
    want = (want + 0xFF) & ~0xFFull;
    HBlock b = {malloc(want), want, 0};
    if (!b.mem) FATAL("nvn: out of host memory for command records (%llu bytes)\n", (unsigned long long)want);
    return b;
}

static void shadow_kill(Shadow *sh) {  // (lock held; already out of the live list) its records are dead
    for (int i = 0; i < sh->nblocks; i++) block_release(sh->blocks[i]);
    sh->nblocks = 0;
    sh->dead = true;
    if (!sh->users) { free(sh->blocks); free(sh); }
}

static void shadow_unuse(Shadow *sh) {  // (lock held)
    if (!--sh->users && sh->dead) { free(sh->blocks); free(sh); }
}

static int live_index(uint64_t lo) {  // (lock held) the first live shadow not wholly before lo
    int a = 0, b = g_nlive;
    while (a < b) {
        int m = (a + b) / 2;
        if (g_live[m]->hi <= lo && g_live[m]->lo < lo) a = m + 1; else b = m;
    }
    return a;
}

// A command buffer moves from shadow `old` (NULL: none yet), having used `used` guest bytes there, to the shadow
// for guest memory [lo, hi): the same one when the game adds exactly that range again, unless that is the one
// being recorded into (`recording`: its blocks still hold the start of the list); otherwise a new one, and the
// shadows its first bytes go over die (shadow_claim: the others once written over). What `old` stands for shrinks
// to the part used first.
static Shadow *shadow_switch(Shadow *old, uint64_t used, bool recording, uint64_t lo, uint64_t hi) {
    pthread_mutex_lock(&g_shadow_lock);
    if (old && !old->dead && old->lo < old->hi && old->lo + used < old->hi) old->hi = old->lo + used;
    Shadow *sh = NULL;
    if (lo < hi) {
        int a = live_index(lo);
        Shadow *same = a < g_nlive && g_live[a]->lo == lo && g_live[a]->win == hi ? g_live[a] : NULL;
        if (same && (same->users == (same == old)) && !(recording && same == old)) sh = same;
        else {
            int end = a;
            while (end < g_nlive && g_live[end]->lo <= lo) end++;
            for (int i = a; i < end; i++) shadow_kill(g_live[i]);
            if (g_nlive - (end - a) + 1 > g_caplive) {
                g_caplive = g_caplive ? g_caplive * 2 : 1024;
                g_live = realloc(g_live, g_caplive * sizeof *g_live);
            }
            memmove(g_live + a + 1, g_live + end, (g_nlive - end) * sizeof *g_live);
            g_nlive += 1 - (end - a);
            sh = calloc(1, sizeof *sh);
            sh->lo = sh->hi = lo;
            sh->win = hi;
            sh->free_to = a + 1 < g_nlive && g_live[a + 1]->lo < hi ? g_live[a + 1]->lo : hi;
            g_live[a] = sh;
            if (a > 0 && g_live[a - 1]->free_to > lo) g_live[a - 1]->free_to = lo;
        }
    } else {
        sh = calloc(1, sizeof *sh);
    }
    sh->users++;
    if (old) shadow_unuse(old);
    pthread_mutex_unlock(&g_shadow_lock);
    return sh;
}

// The command buffer recording into `sh` has written its guest memory up to `end`: lists recorded there before die.
static void shadow_claim(Shadow *sh, uint64_t end) {
    if (end > sh->win) end = sh->win;  // (recording goes on past the window when the game adds no more: accounting only)
    if (end <= sh->hi || sh->dead) return;
    if (end <= sh->free_to) { sh->hi = end; return; }
    pthread_mutex_lock(&g_shadow_lock);
    if (!sh->dead) {
        int i = live_index(sh->lo);
        while (i < g_nlive && g_live[i] != sh) i++;
        if (i < g_nlive) {
            int last = i + 1;
            while (last < g_nlive && g_live[last]->lo < end) last++;
            for (int k = i + 1; k < last; k++) shadow_kill(g_live[k]);
            memmove(g_live + i + 1, g_live + last, (g_nlive - last) * sizeof *g_live);
            g_nlive -= last - (i + 1);
            sh->hi = end;
            sh->free_to = i + 1 < g_nlive && g_live[i + 1]->lo < sh->win ? g_live[i + 1]->lo : sh->win;
        }
    }
    pthread_mutex_unlock(&g_shadow_lock);
}

static void shadow_leave(Shadow *sh, uint64_t used) {  // (a command buffer is finalized)
    pthread_mutex_lock(&g_shadow_lock);
    if (!sh->dead && sh->lo < sh->hi && sh->lo + used < sh->hi) sh->hi = sh->lo + used;
    shadow_unuse(sh);
    pthread_mutex_unlock(&g_shadow_lock);
}

// A memory pool is finalized: the lists recorded in its memory [lo, hi) are dead.
void cmdbuf_pool_gone(uint64_t lo, uint64_t hi) {
    pthread_mutex_lock(&g_shadow_lock);
    int a = live_index(lo), end = a;
    while (end < g_nlive && g_live[end]->lo < hi) end++;
    for (int i = a; i < end; i++) shadow_kill(g_live[i]);
    memmove(g_live + a, g_live + end, (g_nlive - end) * sizeof *g_live);
    g_nlive -= end - a;
    pthread_mutex_unlock(&g_shadow_lock);
}

// Block i of a shadow, at least `want` bytes (blocks past the one in use hold dead records).
static HBlock shadow_block(Shadow *sh, int i, uint64_t want) {
    pthread_mutex_lock(&g_shadow_lock);
    if (i < sh->nblocks && sh->blocks[i].size < want) {
        block_release(sh->blocks[i]);
        sh->blocks[i] = block_get(want);
    }
    while (sh->nblocks <= i) {
        if (sh->nblocks == sh->cap) {
            sh->cap = sh->cap ? sh->cap * 2 : 4;
            sh->blocks = realloc(sh->blocks, sh->cap * sizeof *sh->blocks);
        }
        HBlock nb = block_get(want);  // (before the count goes up: the stats line reads the blocks)
        sh->blocks[sh->nblocks++] = nb;
    }
    HBlock b = sh->blocks[i];
    pthread_mutex_unlock(&g_shadow_lock);
    return b;
}

RT_ON_RESET(cmdbuf_shadow_reset) {  // (reboot: every list is dead; command buffers were guest objects)
    pthread_mutex_lock(&g_shadow_lock);
    for (int i = 0; i < g_nlive; i++) {
        for (int k = 0; k < g_live[i]->nblocks; k++) free(g_live[i]->blocks[k].mem);
        free(g_live[i]->blocks);
        free(g_live[i]);
    }
    g_nlive = 0;
    for (int i = 0; i < g_ngrave; i++) free(g_grave[i].mem);
    g_ngrave = 0;
    pthread_mutex_unlock(&g_shadow_lock);
}

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    RET(1);
}
NVN(nvnCommandBufferFinalize) {
    NvnCmdBuf *cb = cb_of(A(0));
    if (cb && cb->shadow) shadow_leave(cb->shadow, cb->gused);
    free(cb);
    SET_HOST(A(0), 0);
}
NVN(nvnCommandBufferSetDebugLabel) { (void)c; }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

// Record into block i of the command buffer's shadow, chaining the list being recorded on to it.
static void use_block(NvnCmdBuf *cb, int i, uint64_t want) {
    HBlock b = shadow_block(cb->shadow, i, want);
    uint64_t h = (uint64_t)(uintptr_t)b.mem;
    if (cb->recording && cb->mem) {  // (rec_cost always leaves JUMP_RESERVE free for this)
        CmdRec *j = raw_rec(cb->mem + cb->used, CMD_JUMP, sizeof(CmdRec));
        j->x[0] = h;
    }
    cb->block = i;
    cb->mem = h;
    cb->size = b.size;
    cb->used = 0;
}

static void use_guest_chunk(NvnCmdBuf *cb, uint64_t gmem, uint64_t gsize) {
    cb->shadow = shadow_switch(cb->shadow, cb->gused, cb->recording, gmem, gmem ? gmem + gsize : 0);
    cb->gmem = gmem;
    cb->gsize = gsize;
    cb->gused = 0;
    uint64_t want = gsize * 8;  // (records take several times the GPU bytes; more blocks chain on when needed, each
    use_block(cb, 0, want < 0x400 ? 0x400 : want > 0x4000 ? 0x4000 : want);  // twice the last: start small)
}

NVN(nvnCommandBufferAddCommandMemory) {  // (cmd, pool, offset, size)
    NvnCmdBuf *cb = cb_of(A(0));
    NvnPool *pool = A(1) ? HOST(NvnPool, A(1)) : NULL;
    if (!pool) {
        static int logged;
        if (logged++ < 4) LOG("[nvn] AddCommandMemory(%llx) without a memory pool (offset %llx, size %llx)\n",
                              (unsigned long long)A(0), (unsigned long long)A(2), (unsigned long long)A(3));
    }
    use_guest_chunk(cb, pool ? pool->storage + A(2) : 0, A(3));
}
NVN(nvnCommandBufferAddControlMemory) { (void)c; }
NVN(nvnCommandBufferGetCommandMemorySize) { RET(cb_of(A(0))->gsize); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { RET(cb_of(A(0))->gused); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->gsize > cb->gused ? cb->gsize - cb->gused : 0);
}
NVN(nvnCommandBufferGetControlMemorySize) { RET(0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { RET(0); }
NVN(nvnCommandBufferGetControlMemoryFree) { RET(0x10000); }
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { (void)c; }

// Reserve a record costing `cost` guest bytes. May call back into the guest for more memory, so callers snapshot
// their arguments first. When the game adds no more (BOTW gives some command buffers a callback that is a bare
// ret), recording goes on past the guest chunk: only the accounting is over, the records have room.
static CmdRec *rec_cost(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra, uint32_t cost) {
    uint32_t size = (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u);
    for (int tries = 0; !cb->shadow || cb->gused + cost > cb->gsize; tries++) {
        if (!cb->callback || tries == 2) {
            if (!cb->shadow) use_guest_chunk(cb, 0, 0);  // never given memory: the records still need a home
            break;
        }
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->guest;
        c->x[1] = 0;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY
        c->x[2] = cost;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    cb->gused += cost;
    shadow_claim(cb->shadow, cb->gmem + cb->gused);
    if (cb->used + size + JUMP_RESERVE > cb->size) {
        uint64_t want = size + JUMP_RESERVE;
        use_block(cb, cb->block + 1, want > cb->size * 2 ? want : cb->size * 2);
    }
    CmdRec *r = raw_rec(cb->mem + cb->used, op, size);
    cb->used += size;
    return r;
}
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    return rec_cost(c, cb, op, extra, cmd_cost(op, extra));
}

NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->recording = true;
    rec_cost(c, cb, CMD_BARRIER, 0, 0);  // makes sure memory exists; the list starts here
    cb->start = cb->mem + cb->used - sizeof(CmdRec);
}
NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    rec_cost(c, cb, CMD_END, 0, 0);
    cb->recording = false;
    RET(cb->start);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len) {
    Snap s = snap(c);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnProgram, s.x[1]);  // host program
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
// The same arguments, but the handles are read when the commands run (BOTW binds material textures this way)
NVN(nvnCommandBufferBindTexturesDeferred) { Snap s = snap(c); record(c, CMD_BIND_TEXTURES_DEFERRED, 0, &s); }
NVN(nvnCommandBufferBindTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindImage) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
// Entry points the renderer does not implement: logged on first use, so a game that needs one shows up in the log.
#define UNIMPLEMENTED_ONCE(what) \
    do { \
        static int told_; \
        if (!told_++) LOG("[nvn] CommandBuffer%s called: not implemented\n", what); \
    } while (0)
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateSamplers) { (void)c; UNIMPLEMENTED_ONCE("BindSeparateSamplers"); }
NVN(nvnCommandBufferBindSeparateSampler) { (void)c; UNIMPLEMENTED_ONCE("BindSeparateSampler"); }
NVN(nvnCommandBufferBindTransformFeedbackBuffers) { (void)c; UNIMPLEMENTED_ONCE("BindTransformFeedbackBuffers"); }
NVN(nvnCommandBufferBindTransformFeedbackBuffer) { (void)c; UNIMPLEMENTED_ONCE("BindTransformFeedbackBuffer"); }
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
// Textures in records are host textures, taken when recorded (nvn_texture_host): on hardware a list holds the
// texture's address and shape as they were then. One the game finalizes later (BOTW does, between frames, for render
// targets its display lists keep using) still names its memory: the renderer keeps one image per memory and shape.
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap(c);
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    for (int i = 0; i < n; i++) {
        rt.tex[i] = colors ? (uint64_t)(uintptr_t)nvn_texture_host(colors[i]) : 0;
        if (views && views[i]) { rt.has_view[i] = 1; memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView)); }
    }
    rt.depth = (uint64_t)(uintptr_t)nvn_texture_host(s.x[4]);
    if (s.x[5]) { rt.has_dview = 1; memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView)); }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8); }
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16); }
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS)
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)
NVN(nvnCommandBufferSetRenderEnableConditional) { (void)c; UNIMPLEMENTED_ONCE("SetRenderEnableConditional"); }
PLAIN(nvnCommandBufferSetDepthClamp, CMD_SET_DEPTH_CLAMP)
PLAIN(nvnCommandBufferSetRasterizerDiscard, CMD_SET_RASTERIZER_DISCARD)
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN(nvnCommandBufferSetPatchSize) { (void)c; }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; }
NVN(nvnCommandBufferSaveZCullData) { (void)c; }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap(c);
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap(c);
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16); }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2; }
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap(c);
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)nvn_texture_host(s.x[1]);
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2); }
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER)
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (SAVAGEWIND_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = getenv("SAVAGEWIND_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; }

// ---- draws (all integer arguments fit in x1..x7)
NVN(nvnCommandBufferDrawArrays) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { Snap s = snap(c); record(c, CMD_DRAW_ARRAYS, 0, &s); }
// Debug: SAVAGEWIND_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = getenv("SAVAGEWIND_TRACE_COUNT") ? atol(getenv("SAVAGEWIND_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 40) return;
    LOG("[trace] draw count %llu lr %llx chain:", (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[5] = 0; r->x[6] = 1;
}
NVN(nvnCommandBufferDrawElementsInstanced) { trace_draw(c, c->x[3]); Snap s = snap(c); record(c, CMD_DRAW_ELEMENTS, 0, &s); }
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT)
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT)
NVN(nvnCommandBufferMultiDrawArraysIndirectCount) { (void)c; UNIMPLEMENTED_ONCE("MultiDrawArraysIndirectCount"); }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount) { (void)c; UNIMPLEMENTED_ONCE("MultiDrawElementsIndirectCount"); }
NVN(nvnCommandBufferDrawTransformFeedback) { (void)c; UNIMPLEMENTED_ONCE("DrawTransformFeedback"); }
NVN(nvnCommandBufferDrawTexture) { (void)c; UNIMPLEMENTED_ONCE("DrawTexture"); }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)
PLAIN(nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT)
NVN(nvnCommandBufferBeginTransformFeedback) { (void)c; UNIMPLEMENTED_ONCE("BeginTransformFeedback"); }
NVN(nvnCommandBufferEndTransformFeedback) { (void)c; UNIMPLEMENTED_ONCE("EndTransformFeedback"); }
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; UNIMPLEMENTED_ONCE("PauseTransformFeedback"); }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; UNIMPLEMENTED_ONCE("ResumeTransformFeedback"); }

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = (uint64_t)(uintptr_t)nvn_texture_host(tex);
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER)
NVN(nvnCommandBufferDownsample) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) { record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4)); }

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap(c);
    record(c, CMD_FENCE_SYNC, 0, &s);
    extern void nvn_sync_in_list(uint64_t guest);
    nvn_sync_in_list(A(1));
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER)
NVN(nvnCommandBufferReportValue) { (void)c; UNIMPLEMENTED_ONCE("ReportValue"); }
PLAIN(nvnCommandBufferResetCounter, CMD_RESET_COUNTER)  // (BOTW: before its sun occlusion query)
NVN(nvnCommandBufferCallCommands) { record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8); }
NVN(nvnCommandBufferCopyCommands) { record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8); }
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; }
