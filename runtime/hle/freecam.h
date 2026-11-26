#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREECAM_MAX_KEYFRAMES 32

typedef struct {
    float eye[3];
    float yaw;
    float pitch;
} FreecamKeyframe;

bool freecam_active(void);
void freecam_toggle(void);
void freecam_key(int scancode, bool down);
void freecam_pad(float lx, float ly, float rx, float ry, float up, float down, bool fast, bool slow);
void freecam_mouse(float dx, float dy);
void freecam_frame(void);

bool freecam_path_key(int scancode);
void freecam_pad_buttons(uint32_t buttons);
void freecam_keyframe_add(void);
void freecam_keyframe_pop(void);
void freecam_keyframe_clear(void);
void freecam_path_toggle(void);
void freecam_path_loop_toggle(void);
void freecam_path_step_duration(float delta_sec);

bool freecam_path_playing(void);
bool freecam_path_loop(void);
int freecam_keyframe_count(void);
float freecam_path_duration(void);

// Evaluate a Catmull-Rom spline camera path at normalised time t_norm in [0, 1].
void freecam_eval_path(const FreecamKeyframe *kf, int n, float t_norm, bool loop, float out_eye[3], float *out_yaw,
                       float *out_pitch);

#ifdef __cplusplus
}
#endif
