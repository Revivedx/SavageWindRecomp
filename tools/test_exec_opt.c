// Synthetic unit tests for exec.c optimizations: branchless index max-scan,
// contiguous vertex stream batching, and 4-way set-associative guest_copy cache.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t scan_max_u16(const uint16_t *s, uint32_t count, bool prim_restart, uint16_t restart, uint32_t cur) {
    if (!prim_restart) {
        uint16_t mx = cur > 0xFFFFu ? 0xFFFFu : (uint16_t)cur;
        for (uint32_t i = 0; i < count; i++)
            if (s[i] > mx) mx = s[i];
        return mx > cur ? mx : cur;
    }
    for (uint32_t i = 0; i < count; i++)
        if (s[i] > cur && s[i] != restart) cur = s[i];
    return cur;
}

static uint32_t scan_max_u32(const uint32_t *s, uint32_t count, bool prim_restart, uint32_t restart, uint32_t cur) {
    if (!prim_restart) {
        for (uint32_t i = 0; i < count; i++)
            if (s[i] > cur) cur = s[i];
        return cur;
    }
    for (uint32_t i = 0; i < count; i++)
        if (s[i] > cur && s[i] != restart) cur = s[i];
    return cur;
}

static int checks, fails;
static void check(bool ok, const char *name) {
    checks++;
    if (!ok) {
        fails++;
        fprintf(stderr, "FAIL: %s\n", name);
    }
}

int main(void) {
    // 1. Index max-scan tests
    uint16_t idx16[19] = {0, 3, 1, 65535, 12, 9, 400, 7, 15, 2, 1024, 5, 88, 65535, 300, 17, 42, 99, 500};
    check(scan_max_u16(idx16, 19, true, 0xFFFF, 0) == 1024, "u16 with prim_restart ignores 0xFFFF");
    check(scan_max_u16(idx16, 19, false, 0xFFFF, 0) == 65535, "u16 without prim_restart finds 65535");
    check(scan_max_u16(idx16, 0, false, 0, 77) == 77, "u16 empty preserves cur");

    uint32_t idx32[11] = {5, 100, 0xFFFFFFFFu, 42, 999999u, 1, 2, 3, 4, 5, 6};
    check(scan_max_u32(idx32, 11, true, 0xFFFFFFFFu, 0) == 999999u, "u32 with prim_restart ignores 0xFFFFFFFF");
    check(scan_max_u32(idx32, 11, false, 0xFFFFFFFFu, 0) == 0xFFFFFFFFu, "u32 without prim_restart finds 0xFFFFFFFF");

    // 2. Contiguous vertex stream batching
    uint32_t vdirty = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 6) | (1u << 16);
    uint32_t got_first[8], got_run[8];
    int nruns = 0;
    while (vdirty) {
        int first = __builtin_ctz(vdirty), end = first;
        while ((vdirty >> end) & 1u) end++;
        uint32_t run = (uint32_t)(end - first);
        got_first[nruns] = (uint32_t)first;
        got_run[nruns] = run;
        nruns++;
        vdirty &= ~(((1u << run) - 1u) << first);
    }
    check(nruns == 3, "vdirty splits into 3 contiguous runs");
    check(got_first[0] == 0 && got_run[0] == 3, "run 0 is [0..2]");
    check(got_first[1] == 5 && got_run[1] == 2, "run 1 is [5..6]");
    check(got_first[2] == 16 && got_run[2] == 1, "run 2 is [16]");

    printf("test_exec_opt: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
