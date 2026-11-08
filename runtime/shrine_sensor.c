// Shrine sensor volume / mute (runtime/shrine_sensor.h).
// Patches the AMTA and BWAV sound data of Sys_Item_SheikSensor in Sound/Resource/M_UI.bars
// inside Pack/Bootup.pack (or loose in RomFS). The game reads that file when it starts, so a change counts from the next start.
#include "shrine_sensor.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

bool shrine_sensor_patch_bars(uint8_t *bars, size_t size, int volume_pct) {
    if (size < 16 || memcmp(bars, "BARS", 4) || memcmp(bars + 8, "\xFF\xFE", 2)) return false;
    uint32_t cnt = rd32(bars + 12);
    if (16 + (uint64_t)cnt * 12 > size) return false;

    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t at = rd32(bars + 16 + cnt * 4 + i * 8);
        uint32_t wave = rd32(bars + 20 + cnt * 4 + i * 8);
        if ((uint64_t)at + 0x1C > size || memcmp(bars + at, "AMTA\xFF\xFE", 6)) continue;

        uint32_t data_off = rd32(bars + at + 0xC);
        uint32_t strg_off = rd32(bars + at + 0x18);
        if ((uint64_t)at + strg_off + 8 > size) continue;
        if (memcmp(bars + at + strg_off, "STRG", 4)) continue;

        const char *name = (const char *)bars + at + strg_off + 8;
        if (strcmp(name, "Sys_Item_SheikSensor")) continue;

        uint64_t data = (uint64_t)at + data_off;
        if (data + 0x28 > size || memcmp(bars + data, "DATA", 4)) return false;

        // In AMTA DATA chunk:
        // offset 0x0: "DATA", offset 0x4: size
        // offset 0x8 + 0x1C = 0x24: Volume in dB (float)
        float orig_db;
        memcpy(&orig_db, bars + data + 0x24, 4);

        float new_db = orig_db;
        if (volume_pct <= 0) {
            new_db = -999.0f;
        } else if (volume_pct < 100) {
            float factor = (float)volume_pct / 100.0f;
            new_db = orig_db + 20.0f * log10f(factor);
        }
        memcpy(bars + data + 0x24, &new_db, 4);

        // If muting completely (0%), zero out raw sample data in BWAV if present
        if (volume_pct <= 0 && wave != 0xFFFFFFFFu && (uint64_t)wave + 0x10 <= size &&
            !memcmp(bars + wave, "BWAV", 4)) {
            uint16_t num_ch;
            memcpy(&num_ch, bars + wave + 0xE, 2);
            size_t ch_base = (size_t)wave + 0x10;
            if (ch_base + 0x4C > size || rd32(bars + ch_base + 4) < 8000) {
                ch_base = (size_t)wave + 0x40;
            }
            for (uint16_t ch = 0; ch < num_ch && ch_base + (ch + 1) * 0x4C <= size; ch++) {
                uint32_t data_start = rd32(bars + ch_base + ch * 0x4C + 0x30);
                uint32_t num_samples = rd32(bars + ch_base + ch * 0x4C + 0xC);
                if (data_start && (uint64_t)data_start < size) {
                    size_t bytes = (num_samples + 1) / 2 + 16;
                    if (data_start + bytes > size) bytes = size - data_start;
                    memset(bars + data_start, 0, bytes);
                }
            }
        }
        return true;
    }
    return false;
}
