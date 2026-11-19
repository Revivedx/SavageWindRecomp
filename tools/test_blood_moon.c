// The blood moon request (runtime/blood_moon.c): the game's relief request found in synthetic code, and a request that
// waits for play before it reaches the game. No game files or renderer. make test-blood-moon
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/hle.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/blood_moon.c"

GameSites g_site;
static bool menu_up, title;
bool menu_input_captured(void) { return menu_up; }
bool framepace_title_ui(void) { return title; }
bool framepace_loading_ui(void) { return false; }
bool framepace_paused(void) { return false; }
bool framepace_in_event(void) { return false; }

int main(void) {
    uint32_t code[128] = {0};
    // Accessor at +0x100: singleton at +0x1000, tail call backwards to predicate at +0x20.
    code[0x100 / 4] = 0xb0000008;
    code[0x104 / 4] = 0x91000108;
    code[0x108 / 4] = 0xf9400100;
    code[0x10c / 4] = 0x17ffffc5;
    code[0x34 / 4] = 0xb9417408;
    code[0x38 / 4] = 0xaa0003f4;
    code[0x3c / 4] = 0x37600228;  // tbnz w8, #12, +0x80
    code[0x80 / 4] = 0x12137908;
    code[0x84 / 4] = 0xb9017688;
    assert(find_relief_root(code, sizeof code, MAIN_BASE, MAIN_BASE + 0x1000, MAIN_BASE + 0x2000) == MAIN_BASE + 0x1000);
    code[0x80 / 4] ^= 1;  // reject a branch that does not consume the relief bit
    assert(!find_relief_root(code, sizeof code, MAIN_BASE, MAIN_BASE + 0x1000, MAIN_BASE + 0x2000));
    code[0x80 / 4] ^= 1;
    code[0x3c / 4] = 0x3767ffe8;  // branch beyond the supplied image
    assert(!find_relief_root(code, sizeof code, MAIN_BASE, MAIN_BASE + 0x1000, MAIN_BASE + 0x2000));
    code[0x3c / 4] = 0x37600228;
    assert(!find_relief_root(code, sizeof code, MAIN_BASE, MAIN_BASE + 0x1800, MAIN_BASE + 0x2000));
    assert(!find_relief_root(code, 0x80, MAIN_BASE, MAIN_BASE + 0x1000, MAIN_BASE + 0x2000));
    assert(signed_imm(0x3ffffff, 26) == -1);
    assert(!guest_pointer(0, 1) && !guest_pointer(GUEST_HI - 1, 8));
    assert(guest_pointer(GUEST_LO, 8));

    // No world: refused. On the title: refused. A pending request stays while a menu is up, and a reset drops it.
    assert(!blood_moon_request() && !blood_moon_pending());
    title = true;
    assert(!blood_moon_request());
    title = false;
    atomic_store(&pending, true);
    menu_up = true;
    blood_moon_frame();
    assert(blood_moon_pending());
    relief_root = MAIN_BASE + 0x1000;
    blood_moon_reset();
    assert(!blood_moon_pending() && !relief_root);
    menu_up = false;
    blood_moon_frame();
    puts("blood moon: relief layout, no world, title, waiting behind a menu and reset passed");
}
