#pragma once
#include <stdbool.h>
#include "cpu.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { bool active; unsigned generation; } GyroScene;
bool pc_gyro_scene_active(void);
bool pc_gyro_keyboard_active(void);  // apparatus interaction, independent of the mouse-drag switch
bool pc_gyro_scene_view(GyroScene *out);
void pc_gyro_scene_tick(void);
void rt_gyro_scene_actor(Cpu *c,uint64_t actor,uint64_t control);
#ifdef __cplusplus
}
#endif
