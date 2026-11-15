// Motion controls (runtime/hle/motion.c): the game's six-axis sensor (gyroscope + accelerometer) from the controller's
// own sensors through SDL3 (or the same controller seen twice: Steam Input, DS4Windows), the Steam Deck's HID node
// (Linux), a DSU server, a PC handheld's own sensors, or the mouse while the tilt key (bindings: BIND_TILT) is held.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Gamepad SDL_Gamepad;
typedef union SDL_Event SDL_Event;
typedef uint32_t SDL_JoystickID;

// Before SDL_Init (runtime/nvn/vulkan/device.c): the controllers Steam Input hides are opened for their sensors
void motion_before_sdl_init(void);

// Main thread (runtime/hle/input.c)
// The controllers changed (connected, gone, the one in use, twins found): picks the one whose sensors to take
void motion_pads(SDL_Gamepad *in_use, SDL_Gamepad *const *pads, int n);
void motion_pad_removed(SDL_Gamepad *pad);  // before SDL_CloseGamepad
// A button pressed on any controller (its id, SDL's event time): true when two controllers just became twins (one
// controller the system shows twice; then motion_pads again)
bool motion_pad_pressed(SDL_JoystickID id, uint64_t ns);
bool motion_pads_twins(SDL_JoystickID a, SDL_JoystickID b);
// A controller Steam Input hides from the game, opened for its sensors only: never the one in use
bool motion_pad_sensor_only(SDL_Gamepad *pad);
SDL_JoystickID motion_sensor_pad(void);       // the controller whose sensor events motion_sensor_event takes (0: none)
void motion_poll(void);  // main window loop: nonblocking motion fallback / freshness
void motion_sensor_event(const SDL_Event *e);  // SDL_EVENT_GAMEPAD_SENSOR_UPDATE
void motion_device_sensor_event(const SDL_Event *e);  // SDL_EVENT_SENSOR_UPDATE (a handheld's own sensors)
void motion_tilt_held(bool held);              // the tilt key, pressed or let go
void motion_mouse(float dx, float dy);         // mouse motion while the tilt key is held (pixels)
void motion_mouse_yaw(float steps);            // tilt key + wheel: six degrees per wheel step
// The virtual gyro at a shrine apparatus (runtime/pc_gyro_scene.c): the gyro keys (-1 / 0 / 1 per axis, held; reset:
// recenter), the drag (an incremental quaternion in the virtual controller's axes, the render thread), the sticks
// (-1..1, dt seconds; with a gyroscope only once pushed past half way), and leaving it (lie flat, nothing queued)
void motion_keyboard(int pitch, int roll, int yaw, bool reset);
void motion_gizmo_arcball(float w, float x, float y, float z);
void motion_dualstick(float lx, float ly, float rx, float ry, double dt, bool reset);
void motion_virtual_end(void);

// Scripted runs (SAVAGEWIND_INPUT: TILT_UP / TILT_DOWN / TILT_LEFT / TILT_RIGHT, TURN_UP / TURN_DOWN / TURN_LEFT /
// TURN_RIGHT): a held tilt in degrees and turning speeds in degrees per second, this frame
void motion_script(float pitch_deg, float roll_deg, float pitch_speed, float yaw_speed);

bool motion_has_sensors(void);  // a usable motion source: fresh readings from a controller, the Deck, DSU, a handheld

// Where the motion comes from now (Controls > Motion source's description)
enum { MOTION_FROM_NONE, MOTION_FROM_CONTROLLER, MOTION_FROM_DSU, MOTION_FROM_DECK, MOTION_FROM_DEVICE };
typedef struct {
    int source;         // MOTION_FROM_*
    bool twin;          // a controller: the one behind the controller in use (Steam Input, DS4Windows)
    bool steam_pad;     // none: the controller in use is Steam Input's gamepad, without sensors
    bool off;           // Motion source: Off
    float rate;         // gyroscope readings per second, measured
    char name[96];      // the controller's name
    char in_use[96];    // the controller in use
} MotionStatus;
void motion_status(MotionStatus *out);

#ifdef __cplusplus
}
#endif
