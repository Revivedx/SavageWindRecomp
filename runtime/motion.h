// Motion controls (runtime/hle/motion.c): the game's six-axis sensor (gyroscope + accelerometer) from the controller's
// own sensors through SDL3, a Linux DSU fallback, or the mouse while the tilt key (bindings: BIND_TILT) is held.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Gamepad SDL_Gamepad;
typedef union SDL_Event SDL_Event;

// Main thread (runtime/hle/input.c)
void motion_pad_opened(SDL_Gamepad *pad);  // turns its gyroscope and accelerometer on, when it has them
void motion_pad_closed(void);
void motion_poll(void);  // main window loop: nonblocking motion fallback / freshness
void motion_sensor_event(const SDL_Event *e);  // SDL_EVENT_GAMEPAD_SENSOR_UPDATE
void motion_tilt_held(bool held);              // the tilt key, pressed or let go
void motion_mouse(float dx, float dy);         // mouse motion while the tilt key is held (pixels)
void motion_mouse_yaw(float steps);            // tilt key + wheel: six degrees per wheel step
// The virtual gyro at a shrine apparatus (runtime/pc_gyro_scene.c): the gyro keys (-1 / 0 / 1 per axis, held; reset:
// recenter), the drag (an incremental quaternion in the virtual controller's axes, the render thread), a controller
// without a gyroscope's sticks (-1..1, dt seconds), and leaving it (lie flat, nothing queued)
void motion_keyboard(int pitch, int roll, int yaw, bool reset);
void motion_gizmo_arcball(float w, float x, float y, float z);
void motion_dualstick(float lx, float ly, float rx, float ry, double dt, bool reset);
void motion_virtual_end(void);

// Scripted runs (SAVAGEWIND_INPUT: TILT_UP / TILT_DOWN / TILT_LEFT / TILT_RIGHT, TURN_UP / TURN_DOWN / TURN_LEFT /
// TURN_RIGHT): a held tilt in degrees and turning speeds in degrees per second, this frame
void motion_script(float pitch_deg, float roll_deg, float pitch_speed, float yaw_speed);

bool motion_has_sensors(void);  // usable motion source (Linux: fresh SDL sensor pair or DSU)

#ifdef __cplusplus
}
#endif
