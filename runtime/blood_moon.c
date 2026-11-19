// The game's own blood moon on request (PR #408, Matthew-Doe): the game has a "panic" blood moon it starts by itself
// when its resource memory runs low, a one-shot request bit its scene update checks every frame. Setting that bit
// leaves the timing, the cutscene and the respawn of monsters and materials to the game, exactly as a blood moon it
// started itself: nothing here simulates exhausted memory or touches the clock.
#include <stdatomic.h>
#include "blood_moon.h"
#include "framepace.h"
#include "game_sites.h"
#include "hle.h"

static atomic_bool pending;
static uint64_t relief_root;

bool menu_input_captured(void);

static int64_t signed_imm(uint32_t v, unsigned bits) {
    uint32_t sign = 1u << (bits - 1);
    return (int64_t)(v ^ sign) - sign;
}

// Find the ResourceMgr singleton accessor whose predicate consumes the explicit relief bit.
// Our own 1.9.0 main: +0x156cd98 -> +0x15661c4; the predicate reads +0x174 at
// +0x15661d8, tests bit 12 at +0x15661e0, clears it at +0x1566284 and returns true.
// The scene update at +0xc3ed94 uses this predicate to request the panic blood moon.
// Validate both the test and its consuming branch instead of pinning a version's addresses.
static uint64_t find_relief_root(const uint32_t *code, size_t bytes, uint64_t base,
                                 uint64_t data, uint64_t end) {
    uint64_t found = 0;
    for (size_t i = 0; i + 4 <= bytes / 4; i++) {
        if ((code[i] & 0x9f00001f) != 0x90000008 ||
            (code[i + 1] & 0xffc003ff) != 0x91000108 || code[i + 2] != 0xf9400100 ||
            (code[i + 3] & 0xfc000000) != 0x14000000) continue;
        int64_t target = (int64_t)(4 * (i + 3)) + signed_imm(code[i + 3] & 0x3ffffff, 26) * 4;
        if (target < 0 || (uint64_t)target + 0x20 > bytes) continue;
        size_t j = (size_t)target / 4 + 5;
        if (code[j] != 0xb9417408 || code[j + 1] != 0xaa0003f4 ||
            (code[j + 2] & 0xfff8001f) != 0x37600008) continue;
        int64_t consume = (int64_t)(4 * (j + 2)) + signed_imm((code[j + 2] >> 5) & 0x3fff, 14) * 4;
        if (consume < 0 || (uint64_t)consume + 8 > bytes ||
            code[consume / 4] != 0x12137908 || code[consume / 4 + 1] != 0xb9017688) continue;
        uint32_t imm = ((code[i] >> 5) & 0x7ffff) << 2 | ((code[i] >> 29) & 3);
        uint64_t site = ((base + 4 * i) & ~0xfffull) + signed_imm(imm, 21) * 4096 +
                        ((code[i + 1] >> 10) & 0xfff);
        if (site < data || site > end || end - site < 8) continue;
        if (found && found != site) return 0;
        found = site;
    }
    return found;
}

static bool guest_pointer(uint64_t p, size_t size) {
    return p >= GUEST_LO && p < GUEST_HI && size <= GUEST_HI - p;
}

// The open world is loaded (the world manager's stage is the main field)
static bool in_field(void) {
    if (!g_site.world_mgr) return false;
    uint64_t world = *(uint64_t *)(uintptr_t)g_site.world_mgr;
    return guest_pointer(world, 0x7da) && *(uint8_t *)(uintptr_t)(world + 0x7d9) &&
           *(uint32_t *)(uintptr_t)(world + 0x6b8) == 1;
}

bool blood_moon_request(void) {
    if (framepace_title_ui() || !in_field()) return false;
    atomic_store(&pending, true);
    LOG("[blood-moon] asked for: it starts once play resumes\n");
    return true;
}

bool blood_moon_pending(void) { return atomic_load(&pending); }

void blood_moon_frame(void) {
    if (!atomic_load(&pending)) return;
    // (the Cheats tab and the console are open when it is asked for: it waits for them, and for a loading screen or a
    // cutscene, so the game's own check meets it in play)
    if (framepace_title_ui() || framepace_loading_ui() || framepace_paused() || framepace_in_event() ||
        menu_input_captured() || !in_field())
        return;
    atomic_store(&pending, false);
    if (!relief_root && g_site.main_data > MAIN_BASE && g_site.main_data < g_site.main_end)
        relief_root = find_relief_root((const uint32_t *)(uintptr_t)MAIN_BASE,
                                      g_site.main_data - MAIN_BASE, MAIN_BASE, g_site.main_data, g_site.main_end);
    if (!relief_root) { LOG("[blood-moon] the game's relief request was not found in this version\n"); return; }
    uint64_t mgr = *(uint64_t *)(uintptr_t)relief_root;
    if (!guest_pointer(mgr, 0x178)) return;
    // Bit 12 is the one-shot request, consumed by the game's next relief check. An atomic or: the game may change the
    // word's other bits meanwhile.
    __atomic_fetch_or((uint32_t *)(uintptr_t)(mgr + 0x174), 1u << 12, __ATOMIC_SEQ_CST);
    LOG("[blood-moon] handed to the game\n");
}

RT_ON_RESET(blood_moon_reset) {
    atomic_store(&pending, false);
    relief_root = 0;
}
