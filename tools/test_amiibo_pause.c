#include <assert.h>
#include "../runtime/hle/framepace.c"

volatile int g_rt_unwind;
static bool picker_open, keyboard_open, stop_requested;
static unsigned presentations, sleeps, close_after;
static uint64_t now, render_time;
static uint32_t native_pause_state;

bool amiibo_picker_active(void) { return picker_open; }
uint64_t plat_now_ns(void) { return now; }
void kernel_sleep_until_ns(uint64_t deadline) {
    assert(deadline > now);
    sleeps++;
    now = deadline;
}
void backend_represent(void) {
    assert(game_frames == 123 && native_pause_state == 2);
    presentations++;
    now += render_time;
    if (keyboard_open && presentations == 2) keyboard_open = false;
    if (presentations == close_after) {
        if (stop_requested) g_rt_unwind = 1;
        else picker_open = false;
    }
}

static void prepare(unsigned frames, uint64_t render_ns) {
    picker_open = true;
    keyboard_open = false;
    stop_requested = false;
    g_rt_unwind = 0;
    presentations = sleeps = 0;
    close_after = frames;
    render_time = render_ns;
    now = 1000000000;
    game_frames = 123;
    native_pause_state = 2;
}

int main(void) {
    prepare(4, 1000000);
    picker_open = false;
    assert(hold_amiibo_picker() == 0 && presentations == 0 && sleeps == 0);
    prepare(4, 1000000);
    keyboard_open = true;
    uint64_t started = now;
    uint64_t held = hold_amiibo_picker();
    assert(held == now - started && held == 3 * 16666667ull + 1000000);
    assert(presentations == 4 && sleeps == 3 && !picker_open && !keyboard_open);
    assert(game_frames == 123 && native_pause_state == 2);
    uint64_t previous_update = started - 8000000;
    previous_update += held;
    assert(now - previous_update == 8000000);
    prepare(2, 25000000);
    assert(hold_amiibo_picker() == 50000000 && sleeps == 0);
    prepare(2, 1000000);
    stop_requested = true;
    assert(hold_amiibo_picker() > 0 && presentations == 2 && g_rt_unwind);
    assert(picker_open);
    prepare(2, 1000000);
    g_rt_unwind = 1;
    assert(hold_amiibo_picker() == 0 && presentations == 0);
    puts("PASS: amiibo selection freezes simulation, presents UI, retains pause state and excludes paused time");
}
