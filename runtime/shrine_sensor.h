// Shrine sensor volume / mute (runtime/shrine_sensor.c).
// Adjusts the volume of the Sheikah Slate shrine and item sensor (Sys_Item_SheikSensor)
// by patching its AMTA metadata and waveform in Sound/Resource/M_UI.bars.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Patch M_UI.bars data in-place to adjust the volume of Sys_Item_SheikSensor.
// volume_pct: 0 = mute (silence), 100 = original volume (no-op), 1..99 = attenuated.
// Returns true if the sound asset was found and parsed.
bool shrine_sensor_patch_bars(uint8_t *bars, size_t size, int volume_pct);

#ifdef __cplusplus
}
#endif

