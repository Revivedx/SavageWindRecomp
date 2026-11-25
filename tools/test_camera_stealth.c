// Standalone behavior checks for Gameplay > Keep crouch after camera (runtime/camera_stealth.c).
// clang -O1 -Wall -Wextra -Werror -Iruntime tools/test_camera_stealth.c -o build/test_camera_stealth
#include <assert.h>
#include <stdio.h>

#include "../runtime/hle.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/camera_stealth.c"

Settings g_settings;

int main(void) {
    const uint64_t player = 0x12345678ull;

    // 1. Option disabled: enter crouched -> exit returns 0 and consumes state.
    g_settings.camera_keep_stealth = false;
    rt_camera_stealth_enter(player, 1);
    assert(rt_camera_stealth_exit(player) == 0);
    g_settings.camera_keep_stealth = true;
    assert(rt_camera_stealth_exit(player) == 0);

    // 2. Option enabled: enter crouched -> exit returns 1 once, second exit returns 0.
    rt_camera_stealth_enter(player, 1);
    assert(rt_camera_stealth_exit(player) == 1);
    assert(rt_camera_stealth_exit(player) == 0);

    // 3. Option enabled: enter standing -> exit returns 0.
    rt_camera_stealth_enter(player, 0);
    assert(rt_camera_stealth_exit(player) == 0);

    // 4. Option enabled: interrupted Camera exit (leave) clears pending crouch.
    rt_camera_stealth_enter(player, 1);
    rt_camera_stealth_leave();
    assert(rt_camera_stealth_exit(player) == 0);

    // 5. Option enabled: new standing entry overwrites stale crouch state.
    rt_camera_stealth_enter(player, 1);
    rt_camera_stealth_enter(player, 0);
    assert(rt_camera_stealth_exit(player) == 0);

    // 6. Player pointer checks: mismatched or zero player cannot restore, and zero clears state.
    rt_camera_stealth_enter(player, 1);
    assert(rt_camera_stealth_exit(player + 0x10) == 0);
    rt_camera_stealth_enter(player, 1);
    assert(rt_camera_stealth_exit(0) == 0);
    rt_camera_stealth_enter(0, 1);
    assert(rt_camera_stealth_exit(player) == 0);

    // 7. Reboot reset clears pending crouch state.
    rt_camera_stealth_enter(player, 1);
    camera_stealth_reset();
    assert(rt_camera_stealth_exit(player) == 0);

    puts("ok: camera stealth enter, exit, leave, pointer mismatch, zero and reset passed");
}
