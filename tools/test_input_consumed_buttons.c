// Contextual cancellation releases only consumed buttons, once, until key-up.
#include <assert.h>
#include "../runtime/hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/hle/input.c"
int main(void) {
    uint64_t back = 1ull << BTN_B, right = 1ull << BTN_RIGHT, attack = 1ull << BTN_Y;
    assert(unconsumed_buttons(right | attack) == (right | attack));
    input_consume_buttons(right | back);
    assert(unconsumed_buttons(right | back | attack) == attack);
    assert(unconsumed_buttons(right | back) == 0); // held: no synthetic re-press
    assert(unconsumed_buttons(right) == 0); // Back released, direction still held
    assert(unconsumed_buttons(back | right) == back); // Back can be used again
    assert(unconsumed_buttons(0) == 0);
    assert(unconsumed_buttons(right | back) == (right | back));
    return 0;
}
