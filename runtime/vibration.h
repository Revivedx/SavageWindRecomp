// HID vibration values, consumed on the SDL event thread (runtime/hle/vibration.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { float low_amplitude, low_frequency, high_amplitude, high_frequency; } VibrationValue;
typedef struct SDL_Gamepad SDL_Gamepad;

void vibration_send(uint32_t handle, const VibrationValue *value);
VibrationValue vibration_get(uint32_t handle);
void vibration_permit(bool permitted);
bool vibration_permitted(void);
void vibration_clear(void);
void vibration_update(SDL_Gamepad *pad, bool focused);
