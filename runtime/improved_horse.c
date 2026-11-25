// Cheats > Improved horse controls (settings.h improved_horse): its steering (runtime/sure_footed.c: its stops).
// The ridden horse's update (tools/patches.py _horse_ride, _horse_stick_scale, _horse_play_rate) steers from the
// stick's x as the rider hands it over: already squared (0.3 sideways arrives as 0.09, less still with the stick
// pushed forward), then scaled per gear (1, 1, 1, .8 at the canter, .5 at the gallop), then smoothed into the
// steering (action +0x268: rate 0.06 into a turn, 0.1 back out, per 30 fps frame, with the frame's delta: the same on
// a Switch). The horse turns only past a steering of about 0.1 (measured: ~170 deg/s per unit above it), so at a
// gallop the stick has to be two-thirds of the way out before anything happens. With the option on the stick's x goes
// in as it is, the steering follows it about three times as fast, and the canter and gallop keep more of it. The
// horse's own steering (roads, obstacles: the turn smoother at +0x278) is left as the game has it.
//
// A turn also slows the horse: it blends the gear's run toward a turning run (the gear's AS, from a steering of 0.1,
// all of it at 0.9) that covers about a third less ground (0.66 of the speed) at the full blend (measured at the
// gallop: 5% lost at 0.25 steering, 20% at 0.57). The steering stores the horse's play rate in its unit (+0x154), and
// the run and the turn both go with it (1.3 gallops and turns ~1.3 times as fast): with the option on it is raised by
// what the turning run loses, so the horse keeps its speed through a turn (measured flat at the gallop and the canter).
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "settings.h"

#define STEER_IN 0.2f  // the steering smoother's rates
#define STEER_OUT 0.3f
#define GAME_STEER_IN 0.06f  // the game's, set once by the action's constructor (1.9.0 0x70006af738)
#define GAME_STEER_OUT 0.1f
static const float k_stick[5] = {1, 1, 1, .93f, .7f};  // the stick's share per gear (game 1, 1, 1, .8, .5)
#define TURN_RUN_LOSS 0.34f  // the speed the turning run loses at the full blend
#define TURN_BLEND_FROM 0.1f  // the steering where the turning run starts to blend in (the gear's AS)
#define TURN_BLEND_FULL 0.9f  // and where it is all of the run

static void put32(uint64_t a, float v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static float get32(uint64_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }

// action: the riding action, one per horse, whose steering rates stay as last written
void rt_horse_ride(uint64_t action) {
    if (g_settings.improved_horse) {
        put32(action + 0x268, STEER_IN);
        put32(action + 0x26c, STEER_OUT);
    } else if (get32(action + 0x268) == STEER_IN && get32(action + 0x26c) == STEER_OUT) {
        put32(action + 0x268, GAME_STEER_IN);  // (a horse ridden while the option was on)
        put32(action + 0x26c, GAME_STEER_OUT);
    }
}

// stick: the stick's x the steering is about to scale (the caller uses what this leaves there)
float rt_horse_stick_scale(int64_t gear, float game, float *stick) {
    if (!g_settings.improved_horse) return game;
    *stick = copysignf(sqrtf(fabsf(*stick)), *stick);
    return gear >= 0 && gear <= 4 ? k_stick[gear] : game;
}

// The play rate the steering stores in the horse's unit every update while it steers.
float rt_horse_play_rate(uint64_t action, float game) {
    if (!g_settings.improved_horse) return game;
    float blend = (fabsf(get32(action + 0x270)) - TURN_BLEND_FROM) / (TURN_BLEND_FULL - TURN_BLEND_FROM);
    blend = blend < 0 ? 0 : blend > 1 ? 1 : blend;
    return game / (1 - TURN_RUN_LOSS * blend);
}
