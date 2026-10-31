// Standalone behavior checks for the experimental shield-surf takeoff grace.
// clang -O1 -ffunction-sections -fdata-sections tools/test_surf_takeoff.c -Wl,--gc-sections -lm -o /tmp/test_surf_takeoff
#include <assert.h>
#include "../runtime/hle/framepace.c"

static void step(uint64_t controller, unsigned flags, float ratio) {
    g_frame_ratio = ratio;
    game_frames++;
    rt_surf_contact(controller, flags);
}

int main(void) {
    setenv("SAVAGEWIND_FRAMEPACE", "1", 1);
    setenv("SAVAGEWIND_SURF_TAKEOFF", "1", 1);
    const uint64_t player = 123;
    const float unchanged[] = {1, 0.5f, 1.0f / 3};
    for (unsigned i = 0; i < sizeof unchanged / sizeof *unchanged; i++) {
        step(player, 2, unchanged[i]);
        assert(rt_surf_jump_request(player));
        assert(!rt_surf_takeoff_pending(player));
    }
    step(player, 2, 0.25f);
    assert(rt_surf_jump_request(player));
    assert(rt_surf_takeoff_pending(player));
    assert(!rt_surf_jump_request(player));  // no repeated impulse in the launch update
    for (int i = 0; i < 3; i++) {
        step(player, 2, 0.25f);
        assert(rt_surf_takeoff_pending(player));  // lingering ground contact preserves upward velocity
        assert(!rt_surf_jump_request(player));
    }
    step(player, 2, 0.25f);
    assert(!rt_surf_takeoff_pending(player));  // obstruction times out after 1/30 second
    assert(rt_surf_jump_request(player));
    step(player, 0, 0.25f);
    assert(!rt_surf_takeoff_pending(player));  // airborne clears the grace immediately
    step(player, 2, 0.25f);
    assert(rt_surf_jump_request(player));
    game_frames += 2;
    rt_surf_contact(player, 2);
    assert(!rt_surf_takeoff_pending(player));  // action stopped updating
    assert(rt_surf_jump_request(player));
    step(456, 2, 0.25f);
    assert(!rt_surf_takeoff_pending(456));  // a different controller cannot inherit protection
    assert(rt_surf_jump_request(456));
    setenv("SAVAGEWIND_SURF_TAKEOFF", "0", 1);
    assert(!rt_surf_takeoff_pending(456));
    assert(rt_surf_jump_request(456));
    puts("ok: surf takeoff baseline rates, grace, duplicate launch, airborne, timeout, reset and disable");
}
