// Settings > Camera (runtime/camera_tune.c): the game's camera, or one of the Classic Camera mod's styles.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// 0 the game's own cameras, else the mod's styles in its order. Their names, for the menus (translated there).
#define CAMERA_STYLES 9
extern const char *const g_camera_style_names[CAMERA_STYLES];

bool camera_pending(void);  // the style the settings have needs a restart (runtime/camera_live.c)

// In the game's camera program (GameRomCamera.baiprog, AAMP): the style's values, as BCML merges the mod. The new
// values go after the program, up to cap, and *size grows; how many values it set.
int camera_style_program(uint8_t *file, size_t *size, size_t cap, int style);
// A number the cameras read: its action (index among the program's Action lists), parameter name crc, AAMP type and value
typedef struct { uint16_t action; uint8_t type, size; uint32_t crc; uint8_t v[12]; } CameraValue;
// Every camera's SInst numbers (not strings), action by action in order; how many (at most max)
int camera_program_values(const uint8_t *file, size_t n, CameraValue *out, int max);
// Each camera's kind (its Def ClassName, "" if none), action by action; how many (at most max)
int camera_program_kinds(const uint8_t *file, size_t n, char (*out)[32], int max);
// The same cameras of the same kinds (each action's Def ClassName): a style that changes kinds needs the camera made again
bool camera_program_same_kinds(const uint8_t *a, size_t an, const uint8_t *b, size_t bn);

// While the game runs (runtime/camera_live.c): fs.c hands over the game's own program and the style it served
void camera_live_init(const uint8_t *vanilla, size_t n, int style);
// The style as the settings have it now: put in the game's cameras at once when it only changes their numbers, else
// (another kind of camera) after a restart. Never blocks: the search runs on its own thread.
void camera_live_apply(int style);
// fs.c: serve the camera actor with this style from now on (the game reads it again on some loads); false if it can't
bool camera_serve_style(int style);

// Room in a SARC after its file at start..end (positions from data_off): grow bytes more, the files after it moved
bool camera_sarc_grow(uint8_t *d, size_t n, size_t cap, uint32_t data_off, uint32_t start, uint32_t end, uint32_t grow);

#ifdef __cplusplus
}
#endif
