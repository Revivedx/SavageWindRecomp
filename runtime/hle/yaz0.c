// BOTW's streaming Yaz0 decoder (1.9.0 0x700131d674, tools/patches.py yaz0_stream) in native code. The game streams most
// of its resources Yaz0-compressed, a chunk at a time, from a loading thread its main thread waits for. Generated, every
// byte went through a volatile guest load and store and a flag computation: at Hyrule Castle most frames over 8 ms while
// running were the main thread waiting for this loop. Same steps, same state, same results as the game's code.
//
// The state (x0): +0 output pointer, +8 bytes still to write (s32), +0xc nonzero: a copy past the end is cut (else
// an error), +0x10 the current group's next flag bit, +0x11 the group's flag byte, +0x12 a copy's first byte,
// +0x14 what the next input byte is (0 a flag byte / literal / a copy's first byte, 1 a copy's second byte, 2 a copy's
// length byte), +0x18 the copy's distance, +0x1a the 16-byte header still to read. x1, w2: the input chunk. Returns the
// bytes still to write, -1 (done with input left over and no cutting), -2 (a copy past the end).
//
// SAVAGEWIND_YAZ0=0: the game's code. =2: both, compared: the native result is worked out on a copy, the game's code
// runs, and its exit (yaz0_stream_ret) checks the state, the result and every byte written ("[yaz0] check").
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu.h"
#include "../rt.h"

static int yaz0_mode(void) {
    static int m = -1;
    if (m < 0) m = getenv("SAVAGEWIND_YAZ0") ? atoi(getenv("SAVAGEWIND_YAZ0")) : 1;
    return m;
}

// The decoder on host pointers: s the 0x1c-byte state, dst its output pointer (s's own +0, or a copy's), src/len the
// chunk. Returns the result; *stored: whether the state was written back (not on the -2 path, as in the game's code).
static uint32_t yaz0_run(uint8_t *s, uint8_t *dst, const uint8_t *src, uint32_t len, bool *stored) {
    uint32_t x0, x13, x10, x9, x12, x14 = 0, x20 = len;
    uint64_t x11;
    uint8_t *x8 = dst;
    memcpy(&x0, s + 8, 4); memcpy(&x13, s + 0x14, 4);
    uint16_t d16; memcpy(&d16, s + 0x18, 2); x11 = d16;
    x10 = s[0x10], x9 = s[0x11], x12 = s[0x12];
    uint32_t cut; memcpy(&cut, s + 0xc, 4);
    *stored = false;
    if ((int32_t)x0 < 1) goto store;
    for (;;) {
        // A whole group at once (flag byte and its 8 literals or copies) while neither the input nor the room left can
        // run out inside it: a group takes at most 1 + 8 * 3 bytes and writes at most 8 * 0x111. It ends in the state
        // the step-by-step loop below would be in: the flag bit used up (0), no copy half read (0), the group's flag
        // byte, the last copy's first byte and distance.
        while (x13 == 0 && x10 == 0 && x20 > 25 && (int32_t)x0 > 8 * 0x111) {
            uint32_t hdr = *src++;
            x20 -= 1;
            if (hdr == 0xff) {  // 8 literals (most of a texture)
                memcpy(x8, src, 8);
                x8 += 8, src += 8, x20 -= 8, x0 -= 8;
                x9 = hdr;
                continue;
            }
            for (uint32_t bit = 0x80; bit; bit >>= 1) {
                if (hdr & bit) {
                    *x8++ = *src++;
                    x20 -= 1, x0 -= 1;
                    continue;
                }
                uint32_t b1 = src[0], n;
                x11 = ((uint64_t)(b1 & 0xf) << 8 | src[1]) + 1;
                if (b1 >> 4) n = (b1 >> 4) + 2, src += 2, x20 -= 2;
                else n = (uint32_t)src[2] + 0x12, src += 3, x20 -= 3;
                x12 = b1;
                const uint8_t *from = x8 - x11;
                // (a short copy as one 16-byte move: the bytes past n are inside the room left and written again later)
                if (x11 >= 16 && n <= 16) memcpy(x8, from, 16), x8 += n;
                else if (x11 >= n) memcpy(x8, from, n), x8 += n;
                else if (x11 == 1) memset(x8, *from, n), x8 += n;
                else { uint32_t k = n; do { *x8++ = *from++; } while (--k); }
                x0 -= n;
            }
            x9 = hdr;
        }
        if (x13 == 1) {  // a copy's second byte: the distance's low 8 bits
            uint32_t first = x12 & 0xff;
            x11 = ((uint64_t)(x12 & 0xf) << 8 | src[0]) + 1;
            if (first < 0x10) { x13 = 2; goto next; }  // (its length in a third byte)
            x14 = (first >> 4) + 2;
        } else if (x13 == 2) {  // a copy's length byte
            x14 = (uint32_t)src[0] + 0x12;
        } else {  // a literal or a copy's first byte, after the group's flag byte when it is used up
            if (x10 == 0) {
                x9 = *src++;
                if (--x20 == 0) { x10 = 0x80; goto store; }
                x10 = 0x80;
            }
            uint32_t b = src[0];
            if (x9 & x10) { *x8++ = (uint8_t)b; x0 -= 1; x14 = x12; }
            else { x13 = 1; x14 = b; }
            x10 >>= 1;
            x12 = x14;
            goto next;
        }
        if (x14 > x0) {  // past the end: cut, or an error (state not written back)
            if (!cut) return 0xfffffffeu;
            x14 = x0 & 0xffff;
        }
        {   // byte by byte where it overlaps what it writes (the same bytes as memcpy where it doesn't, memset at 1)
            uint32_t n = x14;
            const uint8_t *from = x8 - x11;
            if (x11 >= n) memcpy(x8, from, n), x8 += n;
            else if (x11 == 1) memset(x8, *from, n), x8 += n;
            else do { *x8++ = *from++; } while (--n);
        }
        x13 = 0;
        x0 -= x14;
    next:
        if (--x20 == 0) goto store;
        src++;
        if ((int32_t)x0 > 0) continue;
        goto store;
    }
store:
    *stored = true;
    memcpy(s + 8, &x0, 4); memcpy(s + 0x14, &x13, 4);
    uint64_t p = (uint64_t)(uintptr_t)x8; memcpy(s, &p, 8);
    d16 = (uint16_t)x11; memcpy(s + 0x18, &d16, 2);
    s[0x10] = (uint8_t)x10, s[0x11] = (uint8_t)x9, s[0x12] = (uint8_t)x12;
    if (x0 != 0) return x0;
    if (x20 < 0x21 || cut) return 0;
    return 0xffffffffu;
}

// (SAVAGEWIND_YAZ0=2) the native result of the call in progress on this thread, checked at the game code's return
static __thread struct {
    bool on, stored;
    uint8_t *s, *dst;
    uint8_t state[0x1c], before[0x1c];
    uint32_t result;
    uint8_t *out;  // the bytes the native code wrote, from dst
    uint64_t n;
} chk;
static uint64_t chk_calls, chk_bad;

// The decoder's entry: 1 when done here (x0 set; the game's code is skipped), 0 for the game's code (the header still to
// read, an empty chunk, SAVAGEWIND_YAZ0=0, or the check mode).
int rt_yaz0(Cpu *c) {
    int mode = yaz0_mode();
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    const uint8_t *src = (const uint8_t *)(uintptr_t)c->x[1];
    uint32_t len = (uint32_t)c->x[2];
    if (!mode || s[0x1a] || !len) return 0;
    uint64_t d; memcpy(&d, s, 8);
    if (mode != 2) {
        bool stored;
        c->x[0] = yaz0_run(s, (uint8_t *)(uintptr_t)d, src, len, &stored);
        return 1;
    }
    // check mode: on a copy of the state and of the output (and the 4 KB before it, which copies read)
    int32_t rem; memcpy(&rem, s + 8, 4);
    uint64_t room = rem > 0 ? (uint64_t)rem : 0;
    if (room > (64u << 20) || d < GUEST_LO + 0x1000 || d + room > GUEST_HI) return 0;
    uint8_t *buf = malloc(0x1000 + room + 16);
    if (!buf) return 0;
    memcpy(buf, (const void *)(uintptr_t)(d - 0x1000), 0x1000);
    memcpy(chk.before, s, 0x1c);
    memcpy(chk.state, s, 0x1c);
    uint8_t *ndst = buf + 0x1000;
    chk.result = yaz0_run(chk.state, ndst, src, len, &chk.stored);
    uint64_t end; memcpy(&end, chk.state, 8);
    chk.n = chk.stored ? end - (uint64_t)(uintptr_t)ndst : 0;
    if (chk.stored) { uint64_t real = d + chk.n; memcpy(chk.state, &real, 8); }  // (the pointer as the game's code leaves it)
    free(chk.out);
    chk.out = malloc(chk.n + 1);
    if (chk.out) memcpy(chk.out, ndst, chk.n);
    free(buf);
    chk.on = chk.out != NULL, chk.s = s, chk.dst = (uint8_t *)(uintptr_t)d;
    return 0;
}

// The decoder's return (game code): the check mode's comparison
void rt_yaz0_done(Cpu *c) {
    if (!chk.on) return;
    chk.on = false;
    bool ok = (uint32_t)c->x[0] == chk.result;
    if (chk.stored) ok = ok && !memcmp(chk.s, chk.state, 0x1c) && !memcmp(chk.dst, chk.out, chk.n);
    else ok = ok && !memcmp(chk.s, chk.before, 0x1c);
    uint64_t n = __atomic_add_fetch(&chk_calls, 1, __ATOMIC_RELAXED);
    if (!ok) {
        __atomic_add_fetch(&chk_bad, 1, __ATOMIC_RELAXED);
        LOG("[yaz0] check MISMATCH: game result %x native %x, %llu bytes\n", (unsigned)c->x[0], chk.result,
            (unsigned long long)chk.n);
    }
    if (!(n & 1023) || !ok) LOG("[yaz0] check: %llu calls, %llu mismatches\n", (unsigned long long)n,
                                (unsigned long long)chk_bad);
}
