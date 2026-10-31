// Linux, without game files or SDL libraries:
// clang -std=gnu11 -ffunction-sections -fdata-sections -Iruntime -isystem /tmp/sdl3-src/include
//   tools/test_perfmon_position.c runtime/perfmon.c -Wl,--gc-sections -lm -pthread -ldl -o /tmp/test_perfmon_position
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "game_sites.h"
#include "perfmon.h"
#include "rt.h"

GameSites g_site;
volatile int g_rt_unwind, g_movie_playing;
static bool title, loading, event;
static uint64_t heap_size;
uint64_t kernel_heap_size(void) { return heap_size; }
bool framepace_title_ui(void) { return title; }
bool framepace_loading_ui(void) { return loading; }
bool framepace_in_event(void) { return event; }

static void unavailable(void) {
    float pos[3] = {123, 456, 789};
    assert(!perfmon_position(pos));
    assert(pos[0] == 123 && pos[1] == 456 && pos[2] == 789);
}

int main(void) {
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    void *mem = mmap((void *)(uintptr_t)HEAP_BASE, page * 2, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mem == (void *)(uintptr_t)HEAP_BASE);
    assert(!mprotect((char *)mem + page, page, PROT_NONE));
    heap_size = page;
    uint64_t pinfo = 0;
    unavailable();
    g_site.player_info = (uint64_t)(uintptr_t)&pinfo;
    g_site.screen_mgr = g_site.event_mgr = 1;
    unavailable();

    const uint64_t invalid[] = {1, HEAP_BASE - 8, HEAP_BASE + 1, HEAP_BASE + page - 0x90,
                                HEAP_BASE + page, UINT64_MAX - 7};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        pinfo = invalid[i];
        unavailable();
    }
    pinfo = HEAP_BASE;
    heap_size = 0;
    unavailable();
    heap_size = 0x90;
    unavailable();
    heap_size = page;

    // A rejected lifecycle state must not touch even an otherwise in-range, inaccessible object.
    assert(!mprotect(mem, page, PROT_NONE));
    title = true; unavailable(); title = false;
    loading = true; unavailable(); loading = false;
    event = true; unavailable(); event = false;
    g_movie_playing = 1; unavailable(); g_movie_playing = 0;
    g_rt_unwind = 1; unavailable(); g_rt_unwind = 0;
    g_site.screen_mgr = 0; unavailable(); g_site.screen_mgr = 1;
    g_site.event_mgr = 0; unavailable(); g_site.event_mgr = 1;
    assert(!mprotect(mem, page, PROT_READ | PROT_WRITE));

    float pos[3];
    assert(perfmon_position(pos));
    assert(pos[0] == 0 && pos[1] == 0 && pos[2] == 0);
    pinfo = HEAP_BASE + page - 0x98;
    float expected[3] = {-123.4f, 45.6f, -789.0f};
    memcpy((void *)(uintptr_t)(pinfo + 0x88), expected, sizeof expected);
    assert(perfmon_position(pos));
    assert(!memcmp(pos, expected, sizeof pos));
    expected[0] = 432.1f;
    memcpy((void *)(uintptr_t)(pinfo + 0x88), expected, sizeof expected);
    assert(perfmon_position(pos));
    assert(!memcmp(pos, expected, sizeof pos));
    const float invalid_values[] = {NAN, INFINITY, -INFINITY};
    for (int axis = 0; axis < 3; axis++) {
        for (unsigned i = 0; i < sizeof invalid_values / sizeof *invalid_values; i++) {
            memcpy((void *)(uintptr_t)(pinfo + 0x88), expected, sizeof expected);
            memcpy((void *)(uintptr_t)(pinfo + 0x88 + 4 * axis), &invalid_values[i], sizeof(float));
            unavailable();
        }
    }
    assert(!munmap(mem, page * 2));
    puts("position: live reads, lifecycle guards, heap bounds and non-finite values passed");
    return 0;
}
