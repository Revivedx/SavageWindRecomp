// The virtual gyro's keyboard turn and delivery (runtime/hle/motion.c). A gyro key turns the virtual controller while
// it is held; letting go stops the turn and keeps the pose.
#pragma once
#include <math.h>
#include <stdbool.h>

typedef struct {
    double pitch, roll, yaw;  // continuous degrees: wrapped, a sensitivity other than 1 would jump at every turn
    bool active;              // a gyro key, the drag or the sticks moved it since the last recenter
} PcGyro;

// The keys held (-1 / 0 / 1 per axis) over dt seconds: 90 degrees per second
static inline void pc_gyro_step(PcGyro *g, int pitch, int roll, int yaw, double dt) {
    if (pitch || roll || yaw) g->active = true;
    if (!isfinite(dt) || dt <= 0) return;
    if (dt > 0.25) dt = 0.25;  // (a stalled window must not make a large turn)
    g->pitch += pitch * 90 * dt;
    g->roll += roll * 90 * dt;
    g->yaw += yaw * 90 * dt;
}

// Move `current` toward `target` (radians) over one sample of dt seconds, at most 1440 degrees per second across the
// three axes: a fast mouse move or a recenter can ask for several turns at once; they are made, in their direction,
// instead of being wrapped away or handed to the game as one impossible jump.
static inline void pc_gyro_follow(double current[3], const double target[3], double dt) {
    double delta[3], distance = 0;
    for (int i = 0; i < 3; i++) {
        delta[i] = target[i] - current[i];
        distance += fabs(delta[i]);
    }
    double budget = 8 * 3.14159265358979323846 * dt;
    double f = distance > budget ? budget / distance : 1;
    for (int i = 0; i < 3; i++) current[i] += delta[i] * f;
}
