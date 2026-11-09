// The shrine apparatus (runtime/pc_gyro_scene.c): is the player controlling a motion-controlled mechanism?
#pragma once
#include <stdbool.h>

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { bool active; unsigned generation; } GyroScene;  // the drag's view (generation: a change ends a drag)
bool pc_gyro_scene_active(void);    // in the apparatus with the mouse drag on (Controls: Shrine mouse drag)
bool pc_gyro_keyboard_active(void); // in the apparatus, whatever the drag switch: the gyro keys and sticks work
bool pc_gyro_scene_view(GyroScene *out);
void pc_gyro_scene_tick(void);      // game thread, once a frame
void rt_gyro_scene_actor(Cpu *c, uint64_t actor, uint64_t control);  // the game code's heartbeat (tools/patches.py)

#ifdef __cplusplus
}
#endif
