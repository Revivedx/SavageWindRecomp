// Shrine sensor volume / mute (runtime/shrine_sensor.c).
// Adjusts the volume of the Sheikah Slate shrine and item sensor (Sys_Item_SheikSensor)
// by scaling or muting its waveform in Sound/Resource/M_UI.bars.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Patch M_UI.bars data in-place to adjust the volume of Sys_Item_SheikSensor.
// volume_pct: 0 = mute (silence), 100 = original volume (no-op), 1..99 = attenuated.
// Returns true if the sound asset was found and parsed (or modified).
bool shrine_sensor_patch_bars(uint8_t *bars, size_t size, int volume_pct);

// Live volume (the setting applies at once): note the sensor's ADPCM coefficients from an M_UI.bars (returns the
// channels found), then the mixer asks whether a voice's coefficients are the sensor's and scales it by the gain.
int shrine_sensor_note_bars(const uint8_t *bars, size_t size);
bool shrine_sensor_voice(const int16_t coefs[16]);
float shrine_sensor_gain(int volume_pct);
// The sdk renderer: the samples of a voice with these coefficients, where they lie in the game memory, brought to
// volume (0..100) when they are the sensor ones (as shipped or at an earlier volume). true: they were rewritten.
bool shrine_sensor_render(const int16_t coefs[16], uint8_t *samples, size_t size, int volume);

#ifdef __cplusplus
}
#endif
