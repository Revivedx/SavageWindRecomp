// Controls > Mouse bow aiming (settings.h pc_bow): hold the right mouse button to draw and aim, click the left one to
// fire, release the right one to put the bow away. The game's own bow is ZR held to draw and released to fire, and
// B to cancel while ZR is still held; this turns the two mouse buttons into that. Pure state, timed in nanoseconds,
// independent of the frame rate (runtime/hle/input.c samples it each time the game reads the controller).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool aiming;  // the right button is held
    enum { PC_BOW_IDLE, PC_BOW_DRAW, PC_BOW_SHOT, PC_BOW_CANCEL, PC_BOW_SETTLE } phase;
    uint64_t until;  // the end of a timed phase
} PcBow;

// The right button down or up. Up while drawing cancels: B with ZR still held, or releasing ZR would fire.
static inline void pc_bow_aim(PcBow *b, bool down, uint64_t now) {
    b->aiming = down;
    if (down && b->phase == PC_BOW_IDLE) b->phase = PC_BOW_DRAW;
    if (!down && b->phase == PC_BOW_DRAW) { b->phase = PC_BOW_CANCEL; b->until = now + 120000000; }
}

// A left click while drawing: ZR let go for 140 ms (the shot), then drawn again while the right button stays held.
static inline void pc_bow_fire(PcBow *b, uint64_t now) {
    if (b->aiming && b->phase == PC_BOW_DRAW) { b->phase = PC_BOW_SHOT; b->until = now + 140000000; }
}

// The buttons to add to the controller's now (Switch button bits: ZR 9, B 1)
static inline uint64_t pc_bow_buttons(PcBow *b, uint64_t now) {
    const uint64_t zr = 1ull << 9, cancel = 1ull << 1;
    if (b->phase == PC_BOW_SHOT && now >= b->until) b->phase = b->aiming ? PC_BOW_DRAW : PC_BOW_IDLE;
    if (b->phase == PC_BOW_CANCEL && now >= b->until) { b->phase = PC_BOW_SETTLE; b->until = now + 60000000; }
    if (b->phase == PC_BOW_SETTLE && now >= b->until) b->phase = b->aiming ? PC_BOW_DRAW : PC_BOW_IDLE;
    if (b->phase == PC_BOW_CANCEL) return zr | cancel;  // (B while ZR is held: the arrow is put away, not shot)
    if (b->phase == PC_BOW_SETTLE) return cancel;       // (ZR let go after B: nothing fires)
    return b->phase == PC_BOW_DRAW ? zr : 0;
}
