// The clock command against a world whose time reloads from alternating game-data buffers each update.
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/services/test_warp.c"

GameSites g_site;

static unsigned char world[0x600], manager[0x160];
static unsigned char data_manager[0xc30], buffers[2][0x40], flags[2][0x28];
static uint64_t vtable[8], flag_vtables[2][18], wrappers[2], arrays[2][1];
static uint32_t calc_code[32];
static const uint32_t getter_code[2][2] = {{0xbd401000, 0xd65f03c0}, {0xbd401800, 0xd65f03c0}};
static const unsigned value_offsets[2] = {0x10, 0x18};
static uint64_t world_ptr, managers[1];
static float game_time;
static uint64_t frame;
static unsigned pokes;

#define CALC (MAIN_BASE + 0x1000)
#define DATA_SITE (MAIN_BASE + 0x10100)

bool control_mem_peek(uint64_t addr, void *buf, size_t n) {
    if (addr == DATA_SITE && n == 8) {
        uint64_t ptr = (uintptr_t)data_manager;
        memcpy(buf, &ptr, n);
        return true;
    }
    if (addr == CALC && n == sizeof calc_code) { memcpy(buf, calc_code, n); return true; }
    for (int i = 0; i < 2; i++)
        if (addr == CALC + 0x1000 + 8 * i && n == 8) { memcpy(buf, getter_code[i], n); return true; }
#define READABLE(object) if (addr >= (uintptr_t)(object) && n <= sizeof(object) && \
    addr - (uintptr_t)(object) <= sizeof(object) - n) { memcpy(buf, (void *)(uintptr_t)addr, n); return true; }
    READABLE(manager)
    READABLE(data_manager)
    READABLE(buffers)
    READABLE(flags)
    READABLE(vtable)
    READABLE(flag_vtables)
    READABLE(wrappers)
    READABLE(arrays)
#undef READABLE
    return false;
}

bool control_mem_poke(uint64_t addr, const void *buf, size_t n) {
    bool flag = addr >= (uintptr_t)flags && n <= sizeof flags && addr - (uintptr_t)flags <= sizeof flags - n;
    bool gate = addr == (uintptr_t)manager + 0x14e && n == 1;
    bool speed = addr == (uintptr_t)manager + 0xc4 && n == 4;
    if (!flag && !gate && !speed) return false;
    pokes++;
    memcpy((void *)(uintptr_t)addr, buf, n);
    return true;
}

static void expect(float hour, bool held) {
    bool actual_held;
    assert(fabsf(test_warp_hour(&actual_held) - hour) < 0.0001f);
    assert(actual_held == held);
}

static float speed(void) {
    float value;
    memcpy(&value, manager + 0xc4, sizeof value);
    return value;
}

static void tick(float delta) {
    // A buffer swap must retain the held value, and release must leave the native clock in charge.
    uint64_t swap = wrappers[0];
    wrappers[0] = wrappers[1];
    wrappers[1] = swap;
    int source = frame % 2;
    memcpy(&game_time, flags[source] + value_offsets[source], sizeof game_time);
    game_time = fmodf(game_time + delta * speed(), 360.0f);
    if (!manager[0x14e] && game_time >= 165) game_time = 165;
    for (int i = 0; i < 2; i++) memcpy(flags[i] + value_offsets[i], &game_time, sizeof game_time);
    memcpy(manager + 0xb8, &game_time, sizeof game_time);
    test_warp_frame(++frame);
}

int main(int argc, char **argv) {
    (void)argv;
    bool held;
    assert(test_warp_hour(&held) < 0);
    world_ptr = (uintptr_t)world;
    managers[0] = (uintptr_t)manager;
    uint32_t count = 1;
    uint64_t array = (uintptr_t)managers;
    memcpy(world + 0x5c0, &count, sizeof count);
    memcpy(world + 0x5c8, &array, sizeof array);
    g_site.world_mgr = (uintptr_t)&world_ptr;
    g_site.main_data = MAIN_BASE + 0x10000;
    g_site.main_end = MAIN_BASE + 0x20000;
    uint64_t vt = (uintptr_t)vtable;
    memcpy(manager, &vt, sizeof vt);
    vtable[6] = CALC;
    // ADRP x8, DATA_SITE's page; ADD x8, x8, its low 12 bits; LDR x20, [x8]; CBNZ x20.
    uint32_t pages = (DATA_SITE - CALC) >> 12;
    calc_code[0] = 0x90000008 | (pages & 3) << 29 | (pages >> 2) << 5;
    calc_code[1] = 0x91000108 | (DATA_SITE & 0xfff) << 10;
    calc_code[2] = 0xf9400114;
    calc_code[3] = 0xb5000034;
    float initial_speed = 0.25f;
    memcpy(manager + 0xc4, &initial_speed, sizeof initial_speed);
    game_time = 165;
    for (int i = 0; i < 2; i++) {
        wrappers[i] = (uintptr_t)buffers[i];
        arrays[i][0] = (uintptr_t)flags[i];
        uint64_t wrapper = (uintptr_t)&wrappers[i], array_ptr = (uintptr_t)arrays[i];
        uint64_t flag_vt = (uintptr_t)flag_vtables[i];
        flag_vtables[i][0x88 / 8] = CALC + 0x1000 + 8 * i;
        memcpy(flags[i], &flag_vt, sizeof flag_vt);
        uint32_t hash = crc32_of("WM_Time");
        memcpy(data_manager + 0xbd8 + 8 * i, &wrapper, sizeof wrapper);
        memcpy(buffers[i] + 0x28, &count, sizeof count);
        memcpy(buffers[i] + 0x30, &array_ptr, sizeof array_ptr);
        memcpy(flags[i] + value_offsets[i], &game_time, sizeof game_time);
        if (i == 1) memcpy(flags[i] + 0x1c, &hash, sizeof hash);
    }
    uint64_t primary = (uintptr_t)flags[1];
    memcpy(flags[0] + 8, &primary, sizeof primary);
    tick(0);
    if (argc > 1) {
        expect(18, true);
        tick(1);
        expect(18, true);
        assert(game_time == 270 && speed() == 0 && manager[0x14e] == 1);
        test_warp_hold_hour(-1);
        assert(speed() == initial_speed);
        tick(1);
        expect(18 + 0.25f / 15, false);
        puts("hour: environment native-clock hold passed");
        return 0;
    }
    expect(11, false);
    assert(!pokes && manager[0x14e] == 0 && speed() == initial_speed);
    test_warp_set_hour(10);
    expect(10, false);
    tick(1);
    expect(10 + 0.25f / 15, false);
    test_warp_hold_hour(test_warp_hour(NULL));
    float frozen = test_warp_hour(NULL);
    assert(speed() == 0);
    unsigned frozen_pokes = pokes;
    for (int i = 0; i < 100; i++) {
        tick(1);
        expect(frozen, true);
        assert(fabsf(game_time / 15 - frozen) < 0.0001f);
    }
    assert(pokes == frozen_pokes);
    // Setting a frozen clock preserves its state and updates both backing buffers immediately.
    test_warp_set_hour(15);
    expect(15, true);
    tick(1);
    assert(game_time == 225 && speed() == 0);
    test_warp_hold_hour(-1);
    expect(15, false);
    assert(speed() == initial_speed);
    tick(1);
    expect(15 + 0.25f / 15, false);
    unsigned released_pokes = pokes;
    for (int i = 0; i < 1000; i++) tick(1);
    assert(pokes == released_pokes);
    expect(fmodf(225.25f + 250, 360) / 15, false);
    // Paused presentation frames do not accumulate an offset or change native speed.
    float paused = test_warp_hour(NULL);
    for (int i = 0; i < 60; i++) test_warp_frame(++frame);
    expect(paused, false);
    test_warp_hold_hour(paused);
    for (int i = 0; i < 60; i++) test_warp_frame(++frame);
    expect(paused, true);
    test_warp_hold_hour(-1);
    tick(1);
    expect(paused + 0.25f / 15, false);
    // Multiple commands in one batch, including repeated freeze, must not save zero as the native speed.
    test_warp_hold_hour(6);
    test_warp_hold_hour(6);
    test_warp_set_hour(7);
    test_warp_hold_hour(-1);
    expect(7, false);
    assert(speed() == initial_speed);
    tick(1);
    expect(7 + 0.25f / 15, false);
    test_warp_hold_hour(-1);
    tick(1);
    expect(7 + 0.5f / 15, false);
    test_warp_set_hour(23.99f);
    tick(1);
    expect((23.99f * 15 + 0.25f - 360) / 15, false);
    test_warp_hold_hour(24);
    tick(1);
    expect(24, true);
    assert(game_time == 0);
    test_warp_hold_hour(-1);
    expect(0, false);
    tick(1);
    expect(0.25f / 15, false);
    test_warp_set_hour(24);
    expect(0, false);
    // Preserve a nondefault native speed as well.
    float custom_speed = 0.125f;
    memcpy(manager + 0xc4, &custom_speed, sizeof custom_speed);
    test_warp_hold_hour(17);
    tick(1);
    assert(game_time == 255);
    test_warp_hold_hour(-1);
    assert(speed() == custom_speed);
    tick(1);
    expect(17 + custom_speed / 15, false);
    // An unrelated flag at the same handle must never be overwritten.
    uint32_t bad_hash = 0;
    memcpy(flags[1] + 0x1c, &bad_hash, sizeof bad_hash);
    float before = game_time;
    test_warp_set_hour(5);
    float value;
    memcpy(&value, flags[0] + value_offsets[0], sizeof value);
    assert(value == before);
    // A hold requested before GameDataMgr is ready must retry without leaving native speed at zero.
    test_warp_hold_hour(5);
    assert(speed() == custom_speed);
    uint32_t hash = crc32_of("WM_Time");
    memcpy(flags[1] + 0x1c, &hash, sizeof hash);
    test_warp_frame(++frame);
    tick(1);
    expect(5, true);
    assert(game_time == 75 && speed() == 0);
    test_warp_hold_hour(-1);
    assert(speed() == custom_speed);
    test_warp_reset();
    tick(0);
    expect(game_time / 15, false);
    // A running set before world creation is applied once, without turning into a hold.
    world_ptr = 0;
    test_warp_set_hour(8);
    assert(test_warp_hour(&held) < 0 && !held);
    memcpy(flags[1] + 0x1c, &hash, sizeof hash);
    world_ptr = (uintptr_t)world;
    test_warp_frame(++frame);
    expect(8, false);
    tick(1);
    expect(8 + custom_speed / 15, false);
    puts("hour: running set, native freeze, release, speed, pause and rollover passed");
    return 0;
}
