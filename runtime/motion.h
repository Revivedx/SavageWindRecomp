// Motion controls (runtime/hle/motion.c): the game's six-axis sensor (gyroscope + accelerometer) from the controller's
// own sensors through SDL3, or from the mouse while the tilt key (bindings: BIND_TILT) is held.
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
void motion_sensor_event(const SDL_Event *e);  // SDL_EVENT_GAMEPAD_SENSOR_UPDATE
void motion_tilt_held(bool held);              // the tilt key, pressed or let go
void motion_mouse(float dx, float dy);         // mouse motion while the tilt key is held (pixels)
void motion_mouse_yaw(float steps);             // tilt key + wheel: six degrees per wheel unit
void motion_keyboard(int pitch, int roll, int yaw, bool reset);  // -1/0/1 per axis; retain angle on release
typedef struct {
    float matrix[3][3], angles[3]; // controller -> world, then pitch/roll/yaw in degrees
    bool held, pending;
} MotionView;
bool motion_view(MotionView *out);             // coherent copy of the pose actually delivered to the game
void motion_gizmo_rotate(float pitch, float roll, float yaw); // relative degrees, from the render-thread gizmo
void motion_virtual_end(void); // leaving the apparatus: clear virtual input and queued rotation
void motion_gizmo_arcball(float w,float x,float y,float z); // incremental quaternion in virtual-controller axes

// Scripted runs (SAVAGEWIND_INPUT: TILT_UP / TILT_DOWN / TILT_LEFT / TILT_RIGHT, TURN_UP / TURN_DOWN / TURN_LEFT /
// TURN_RIGHT): a held tilt in degrees and turning speeds in degrees per second, this frame
void motion_script(float pitch_deg, float roll_deg, float pitch_speed, float yaw_speed);

bool motion_has_sensors(void);  // the controller has a gyroscope (else the mouse tilt is the only motion)

#ifdef __cplusplus
}
#endif
