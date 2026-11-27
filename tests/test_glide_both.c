/* The decision logic both patched glide sites share. The Makefile extracts
 * rt_glide_overspeed_dec verbatim from runtime/hle/framepace.c next to this file,
 * so the arithmetic tested here is the runtime's own, not a copy of it.
 * Synthetic floats only: no game data, no save, no generated code. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixtures the extracted function needs (its own file defines g_frame_ratio). */
int env_int(const char *name, int def);
extern float g_frame_ratio;
float rt_glide_overspeed_dec(float v, float dec);

int env_int(const char *name, int def) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

static uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static int checks;

static void check(float v, float dec, float ratio, float want, const char *what) {
    g_frame_ratio = ratio;
    float got = rt_glide_overspeed_dec(v, dec);
    if (bits(got) != bits(want)) {
        fprintf(stderr, "FAIL %s: v=%.9g dec=%.9g ratio=%.9g got=%08x want=%08x\n",
                what, v, dec, ratio, bits(got), bits(want));
        exit(1);
    }
    checks++;
}

int main(void) {
    const float traced_v = 1.493f, traced_dec = 0.002f;  /* the traced OverSpeedDec update */
    const float ratios[] = {0.105f, 0.5f, 1.0f, 1.5f};
    int on = env_int("SAVAGEWIND_GLIDE_DECAY", 1) != 0;  /* default on; =0 is the kill switch */

    if (!on) {
        /* =0 (kill switch): every ratio gives the guest's subtraction, bit for bit. */
        for (unsigned i = 0; i < sizeof ratios / sizeof *ratios; i++)
            check(traced_v, traced_dec, ratios[i], traced_v - traced_dec, "=0: guest v - dec");
        check(0.105f, 0.5f, 0.105f, 0.105f - 0.5f, "=0: other values, opposite sign");
    } else {
        /* Unset or =1 (default on): the decrement follows the frame; at ratio 1 the two paths are
         * bit-identical, preserving the guest's 30 fps behaviour. */
        check(traced_v, traced_dec, 1.0f, traced_v - traced_dec, "on: ratio 1 is guest v - dec");
        check(traced_v, traced_dec, 0.105f, traced_v - traced_dec * 0.105f, "on: ratio 0.105");
        check(traced_v, traced_dec, 0.5f, traced_v - traced_dec * 0.5f, "on: ratio 0.5");
        check(0.105f, 0.5f, 1.5f, 0.105f - 0.5f * 1.5f, "on: ratio 1.5");
    }
    printf("glide both helper: PASS (%d checks, option %s)\n", checks, on ? "on (unset/=1)" : "off (=0)");
    return 0;
}
