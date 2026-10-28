#pragma once
#include <stdbool.h>
#include <stdint.h>

// Pure input state machine, independent of rendering frequency. BOTW's trigger releases the arrow;
// cancel (B) must be observed with the trigger still held before the trigger can be released safely.
typedef struct {
    bool aiming;
    enum { PC_BOW_IDLE, PC_BOW_DRAW, PC_BOW_SHOT, PC_BOW_CANCEL, PC_BOW_SETTLE } phase;
    uint64_t until;
} PcBow;

static inline void pc_bow_aim(PcBow *b, bool down, uint64_t now) {
    b->aiming = down;
    if (down && b->phase == PC_BOW_IDLE) b->phase = PC_BOW_DRAW;
    if (!down && b->phase == PC_BOW_DRAW) { b->phase = PC_BOW_CANCEL; b->until = now + 120000000; }
}
static inline void pc_bow_fire(PcBow *b, uint64_t now) {
    if (b->aiming && b->phase == PC_BOW_DRAW) { b->phase = PC_BOW_SHOT; b->until = now + 140000000; }
}
static inline uint64_t pc_bow_buttons(PcBow *b, uint64_t now) {
    const uint64_t zr = 1ull << 9, cancel = 1ull << 1;
    if (b->phase == PC_BOW_SHOT && now >= b->until)
        b->phase = b->aiming ? PC_BOW_DRAW : PC_BOW_IDLE;
    if (b->phase == PC_BOW_CANCEL && now >= b->until) { b->phase = PC_BOW_SETTLE; b->until = now + 60000000; }
    if (b->phase == PC_BOW_SETTLE && now >= b->until)
        b->phase = b->aiming ? PC_BOW_DRAW : PC_BOW_IDLE;
    if (b->phase == PC_BOW_CANCEL) return zr | cancel;
    if (b->phase == PC_BOW_SETTLE) return cancel;
    return b->phase == PC_BOW_DRAW ? zr : 0;
}
