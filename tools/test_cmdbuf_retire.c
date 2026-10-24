// Tests for runtime/nvn/cmdbuf.c: shadows retired once the game writes over their lists' tags, the free pool's
// size classes and budget. No GPU, no game: guest memory is a host buffer, the out-of-memory callback adds more.
//   clang -O1 -g -Iruntime -Iruntime/nvn -I<deps>/include -o build/test_cmdbuf_retire tools/test_cmdbuf_retire.c -lpthread
//   build/test_cmdbuf_retire                              (retiring, the default)
//   SAVAGEWIND_CMD_RETIRE=1 build/test_cmdbuf_retire       (check only: held, late reads caught)
#include "../runtime/nvn/cmdbuf.c"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

// ---- what cmdbuf.c calls outside itself
static uint64_t g_frames;
uint64_t renderer_present_count(void) { return __atomic_load_n(&g_frames, __ATOMIC_SEQ_CST); }
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
bool nvn_timeline(void) { return false; }
uint32_t kernel_current_thread_handle(void) { return 0; }
NvnTexture *nvn_texture_host(uint64_t guest) { (void)guest; return NULL; }
void nvn_sync_in_list(uint64_t guest) { (void)guest; }
void rt_fatal_trace(void) { abort(); }
static uint64_t q_put, q_done;  // the submission queue
uint64_t nvn_queue_put(void) { return __atomic_load_n(&q_put, __ATOMIC_SEQ_CST); }
uint64_t nvn_queue_done(void) { return __atomic_load_n(&q_done, __ATOMIC_SEQ_CST); }

static uint8_t *mem;      // guest memory
static uint64_t pool_obj; // the guest NVNmemoryPool (holds its host NvnPool)
static uint64_t cb_obj;   // the guest NVNcommandBuffer
static uint64_t next_free = 0x10000;  // where the out-of-memory callback adds memory
static Cpu cpu;

static void add_memory(uint64_t offset, uint64_t size) {
    cpu.x[0] = (uint64_t)(uintptr_t)&cb_obj;
    cpu.x[1] = (uint64_t)(uintptr_t)&pool_obj;
    cpu.x[2] = offset;
    cpu.x[3] = size;
    hle_nvnCommandBufferAddCommandMemory(&cpu);
}
void guest_call(Cpu *c, uint64_t addr) {  // the callback: 0x100 more bytes
    (void)c; (void)addr;
    add_memory(next_free, 0x100);
    next_free += 0x100;
}
static void call(void (*fn)(Cpu *)) {
    cpu.x[0] = (uint64_t)(uintptr_t)&cb_obj;
    fn(&cpu);
}
static uint64_t list(int commands) {  // a list with `commands` barriers (8 guest bytes each); its handle
    call(hle_nvnCommandBufferBeginRecording);
    for (int i = 0; i < commands; i++) call(hle_nvnCommandBufferBarrier);
    call(hle_nvnCommandBufferEndRecording);
    return cpu.x[0];
}
static void settle(void) { usleep(80 * 1000); }  // (the sweeper checks every 10 ms)
static int live(void) {
    pthread_mutex_lock(&g_shadow_lock);
    int n = g_nlive;
    pthread_mutex_unlock(&g_shadow_lock);
    return n;
}
static uint64_t retired(void) {
    pthread_mutex_lock(&g_shadow_lock);
    uint64_t n = g_retired;
    pthread_mutex_unlock(&g_shadow_lock);
    return n;
}
static uint64_t late(void) {
    pthread_mutex_lock(&g_held_lock);
    uint64_t n = g_late;
    pthread_mutex_unlock(&g_held_lock);
    return n;
}
static void scribble(uint64_t offset) { memset(mem + offset, 0xAB, 8); }  // the game writes over guest memory

static void test_size_classes(void) {
    for (uint64_t want = 1; want < (1ull << 24); want += want / 7 + 1) {
        uint64_t size, size2;
        int c = size_class(want, &size);
        assert(size >= want && size >= 0x400);
        assert(want <= 0x400 || size <= want + want / 4 + 1);
        assert(size_class(size, &size2) == c && size2 == size);  // a block goes back to its own class
        assert(c >= 0 && c < NCLASS);
    }
}

static void test_queue(void) {  // a dead block is reused once three frames have passed and the queue has run
    pthread_mutex_lock(&g_shadow_lock);
    __atomic_store_n(&q_put, 5, __ATOMIC_SEQ_CST);
    HBlock b = block_get(0x10000);
    uint8_t *mem0 = b.mem;
    block_release(b);
    __atomic_add_fetch(&g_frames, 3, __ATOMIC_SEQ_CST);
    HBlock c = block_get(0x10000);
    assert(c.mem != mem0);  // operations 1..5 still queued
    __atomic_store_n(&q_done, 5, __ATOMIC_SEQ_CST);
    HBlock d = block_get(0x10000);
    assert(d.mem == mem0);
    block_release(c);
    block_release(d);
    pthread_mutex_unlock(&g_shadow_lock);
}

static void test_free_budget(void) {
    pthread_mutex_lock(&g_shadow_lock);
    for (int i = 0; i < 40; i++) block_release(block_get(4ull << 20));  // 160 MB of dead blocks
    __atomic_add_fetch(&g_frames, 3, __ATOMIC_SEQ_CST);
    HBlock b = block_get(0x400);
    assert(g_ngrave == 0 && g_free_bytes <= FREE_BUDGET);
    block_release(b);
    pthread_mutex_unlock(&g_shadow_lock);
}

static int g_stop;
static uint64_t g_late_target;
static void *late_reader(void *p) {  // exec.c's check, from the render thread
    (void)p;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE))
        if (__atomic_load_n(&g_cmd_held, __ATOMIC_ACQUIRE)) nvn_cmd_late(g_late_target);
    return NULL;
}

int main(void) {
    int mode = cmd_retire();
    mem = aligned_alloc(4096, 1 << 20);
    memset(mem, 0, 1 << 20);
    NvnPool pool = {(uint64_t)(uintptr_t)mem, 1 << 20, 0};
    SET_HOST(&pool_obj, &pool);
    call(hle_nvnCommandBufferInitialize);
    NvnCmdBuf *cb = cb_of((uint64_t)(uintptr_t)&cb_obj);
    cb->callback = 1;  // (guest_call above)

    test_size_classes();
    test_queue();
    test_free_budget();

    // Lists packed one after the other, each in a window of its own (BOTW's composite material lists).
    uint64_t expect = 0;
    add_memory(0x0, 0x200);
    uint64_t h1 = list(3);  // guest 0x0..0x18
    uint64_t tag;
    memcpy(&tag, mem, 8);
    assert(tag != 0);  // the first command's bytes hold the tag
    add_memory(0x18, 0x200);
    uint64_t h2 = list(2);  // guest 0x18..0x28
    add_memory(0x1000, 0x200);  // the command buffer moves on
    assert(live() == 3);
    settle();
    assert(live() == 3 && retired() == expect);  // tags intact: nothing retired

    scribble(0x0);  // the game reuses the first list's memory for other data
    settle();
    assert(retired() == ++expect && live() == 2);
    if (mode == 1) {
        assert(late() == 0);
        nvn_cmd_late(h1);  // running the retired list is caught
        assert(late() == 1);
        nvn_cmd_late(h2);  // the next one is still live
        assert(late() == 1);
    }

    // Memory added, nothing recorded: no list can be in it, retired once left (0x1000 here).
    // A window with an empty list: no tag, pinned. Nothing written there retires it.
    add_memory(0x2000, 0x200);
    list(0);
    add_memory(0x3000, 0x200);
    scribble(0x2000);
    settle();
    assert(retired() == ++expect);

    // A list that runs out of memory goes on in the chunk the callback adds: that chunk counts the list's tag, so
    // both go once the list's first command is written over.
    add_memory(0x6000, 0x8);  // room for one command (0x3000: nothing recorded)
    expect++;
    list(3);
    add_memory(0x7000, 0x200);
    settle();
    assert(retired() == expect);
    scribble(0x6000);
    settle();
    expect += 2;
    assert(retired() == expect);

    // The same window added again: a list recorded at the same address replaces the old one's tag.
    add_memory(0x8000, 0x200);  // (0x7000: nothing recorded)
    expect++;
    list(2);
    add_memory(0x9000, 0x200);
    add_memory(0x8000, 0x200);  // again: the same shadow (0x9000: nothing recorded)
    expect++;
    list(1);
    add_memory(0xA000, 0x200);
    settle();
    assert(retired() == expect);
    memcpy(&tag, mem + 0x8000, 8);
    assert(tag != 0);
    scribble(0x8000);
    settle();
    assert(retired() == ++expect);

    // A list whose first command is past the window (the game added no more): accounting only, pinned.
    cb->callback = 0;
    add_memory(0xB000, 0x8);  // (0xA000: nothing recorded)
    expect++;
    list(1);
    list(1);  // past 0xB008
    cb->callback = 1;
    add_memory(0xC000, 0x200);
    scribble(0xB000);
    settle();
    assert(retired() == expect);
    if (mode == 1) assert(late() == 1);

    // The same window added again and left with nothing recorded: the lists there are whole, their tags still count.
    add_memory(0xD000, 0x200);  // (0xC000: nothing recorded)
    expect++;
    list(2);  // 0xD000
    uint64_t second = list(2);  // 0xD010
    add_memory(0xE000, 0x200);
    add_memory(0xD000, 0x200);  // again (0xE000: nothing recorded)
    expect++;
    add_memory(0xF000, 0x200);
    settle();
    assert(retired() == expect);
    scribble(0xD000);
    settle();
    assert(retired() == expect);  // the second list is untouched
    if (mode == 1) {
        nvn_cmd_late(second);
        assert(late() == 1);
    }
    scribble(0xD010);
    settle();
    assert(retired() == ++expect);

    // CopyCommands copies the records in: the list copied can go, the copy stays whole.
    add_memory(0x20000, 0x200);  // (0xF000: nothing recorded)
    expect++;
    uint64_t src = list(2);
    add_memory(0x21000, 0x200);
    call(hle_nvnCommandBufferBeginRecording);
    uint64_t handles[2] = {src, src};
    cpu.x[0] = (uint64_t)(uintptr_t)&cb_obj;
    cpu.x[1] = 2;
    cpu.x[2] = (uint64_t)(uintptr_t)handles;
    hle_nvnCommandBufferCopyCommands(&cpu);
    call(hle_nvnCommandBufferEndRecording);
    uint64_t copy = cpu.x[0];
    add_memory(0x22000, 0x200);
    scribble(0x20000);
    settle();
    assert(retired() == ++expect);
    int barriers = 0, calls = 0;
    for (uint64_t p = copy;;) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op == CMD_END) break;
        if (r->op == CMD_JUMP) {
            p = r->x[0];
            continue;
        }
        barriers += r->op == CMD_BARRIER;
        calls += r->op == CMD_CALL;
        p += r->size;
    }
    assert(calls == 0 && barriers == 2 + 2 * 3);  // its start, the cost; each copy: the start and two commands
    memcpy(&tag, mem + 0x21000, 8);
    assert(tag != 0);  // the copy's own first command (the cost) is tagged

    // A big window still starts with a small block.
    add_memory(0x50000, 0x10000);
    pthread_mutex_lock(&g_shadow_lock);
    assert(cb->shadow->blocks[0].size == 0x400);
    pthread_mutex_unlock(&g_shadow_lock);
    list(200);  // ~16 KB of records: blocks chain on
    pthread_mutex_lock(&g_shadow_lock);
    assert(cb->shadow->nblocks > 1);
    pthread_mutex_unlock(&g_shadow_lock);

    // The memory pool finalized: the shadows in it die.
    add_memory(0x60000, 0x200);
    list(1);
    add_memory(0x61000, 0x200);
    int n = live();
    nvn_cmd_pool_finalized((uint64_t)(uintptr_t)mem + 0x60000, 0x1000);
    assert(live() == n - 1);

    // (mode 1) a list found dead while the render thread runs lists: the check sees it (TSan: no race).
    if (mode == 1) {
        add_memory(0x70000, 0x200);
        g_late_target = list(1);
        add_memory(0x71000, 0x200);
        uint64_t before = late();
        pthread_t t;
        assert(pthread_create(&t, NULL, late_reader, NULL) == 0);
        scribble(0x70000);
        settle();
        __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
        assert(pthread_join(t, NULL) == 0);
        assert(late() > before);
    }

    pthread_mutex_lock(&g_shadow_lock);
    census(rss_pages());
    pthread_mutex_unlock(&g_shadow_lock);
    printf("test_cmdbuf_retire: ok (mode %d)\n", mode);
    return 0;
}
