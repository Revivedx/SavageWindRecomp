// Keyboard tilt is an orientation: releasing a key stops turning, not holding the angle.
#pragma once
#include <stdbool.h>
#include <math.h>

typedef struct {
    double pitch, roll, yaw;  // continuous degrees
    bool active;
} PcGyro;

static inline void pc_gyro_step(PcGyro *g, int pitch, int roll, int yaw, double dt) {
    if (pitch || roll || yaw) g->active = true;
    if (!isfinite(dt) || dt <= 0) return;
    if (dt > 0.25) dt = 0.25;  // a stalled window must not make a large turn
    // Keep continuous angles: wrapping before applying non-integer sensitivity
    // would introduce a discontinuity on every revolution.
    g->pitch += pitch * 90 * dt;
    g->roll += roll * 90 * dt;
    g->yaw += yaw * 90 * dt;
}

// A mouse packet may contain several revolutions. Preserve its winding and
// consume it at a bounded angular rate, rather than wrapping it into no motion
// or presenting one implausible sensor impulse. Angles here are radians.
static inline void pc_gyro_follow(double current[3],const double target[3],double dt) {
    double delta[3],distance=0;
    for(int i=0;i<3;i++){delta[i]=target[i]-current[i];distance+=fabs(delta[i]);}
    double budget=8*3.14159265358979323846*dt;  // 1440 degrees/s across the three axes
    double f=distance>budget ? budget/distance : 1;
    for(int i=0;i<3;i++)current[i]+=delta[i]*f;
}
