// Unit test for shrine sensor volume / mute (runtime/shrine_sensor.c).
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "shrine_sensor.h"

static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wrfloat(uint8_t *p, float v) { memcpy(p, &v, 4); }
static float rdfloat(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

static size_t build_synthetic_bars(uint8_t *buf, size_t cap, float dummy_db, float sensor_db) {
    memset(buf, 0, cap);

    // Offsets in buffer:
    // 0x00: BARS header (0x10 bytes)
    // 0x10: 2 CRC32 IDs (0x8 bytes)
    // 0x18: 2 pairs of {amta_off, bwav_off} (0x10 bytes)
    // Total BARS index: 0x28 bytes
    // 0x40: AMTA 1 (Sys_Dummy)
    // 0xC0: AMTA 2 (Sys_Item_SheikSensor)
    // 0x140: BWAV 2 (Sys_Item_SheikSensor)
    // 0x1B0: Sample data (16 bytes)

    uint32_t amta1_off = 0x40;
    uint32_t amta2_off = 0xC0;
    uint32_t bwav2_off = 0x140;
    uint32_t samples_off = 0x1B0;

    // BARS Header
    memcpy(buf, "BARS", 4);
    wr32(buf + 4, 0x200);  // total size
    memcpy(buf + 8, "\xFF\xFE\x01\x00", 4);  // BOM + version
    wr32(buf + 12, 2);     // count = 2

    // IDs
    wr32(buf + 16, 0x11111111);
    wr32(buf + 20, 0x22222222);

    // Pairs
    wr32(buf + 24, amta1_off);
    wr32(buf + 28, 0xFFFFFFFF);  // dummy has no wave
    wr32(buf + 32, amta2_off);
    wr32(buf + 36, bwav2_off);

    // --- AMTA 1 (Sys_Dummy) ---
    uint8_t *a1 = buf + amta1_off;
    memcpy(a1, "AMTA\xFF\xFE\x01\x00", 8);
    wr32(a1 + 8, 0x70);    // AMTA size
    wr32(a1 + 0xC, 0x20);  // DATA offset from a1
    wr32(a1 + 0x10, 0);    // MARK offset
    wr32(a1 + 0x14, 0);    // EXT_ offset
    wr32(a1 + 0x18, 0x50); // STRG offset from a1

    // AMTA 1 DATA
    memcpy(a1 + 0x20, "DATA", 4);
    wr32(a1 + 0x24, 0x28); // DATA chunk size
    wrfloat(a1 + 0x20 + 0x24, dummy_db);  // volume at DATA + 0x24

    // AMTA 1 STRG
    memcpy(a1 + 0x50, "STRG", 4);
    wr32(a1 + 0x54, 0x10);
    strcpy((char *)a1 + 0x58, "Sys_Dummy");

    // --- AMTA 2 (Sys_Item_SheikSensor) ---
    uint8_t *a2 = buf + amta2_off;
    memcpy(a2, "AMTA\xFF\xFE\x01\x00", 8);
    wr32(a2 + 8, 0x78);    // AMTA size
    wr32(a2 + 0xC, 0x20);  // DATA offset from a2
    wr32(a2 + 0x10, 0);    // MARK offset
    wr32(a2 + 0x14, 0);    // EXT_ offset
    wr32(a2 + 0x18, 0x50); // STRG offset from a2

    // AMTA 2 DATA
    memcpy(a2 + 0x20, "DATA", 4);
    wr32(a2 + 0x24, 0x28); // DATA chunk size
    wrfloat(a2 + 0x20 + 0x24, sensor_db);  // volume at DATA + 0x24

    // AMTA 2 STRG
    memcpy(a2 + 0x50, "STRG", 4);
    wr32(a2 + 0x54, 0x20);
    strcpy((char *)a2 + 0x58, "Sys_Item_SheikSensor");

    // --- BWAV 2 ---
    uint8_t *bw = buf + bwav2_off;
    memcpy(bw, "BWAV", 4);
    wr16(bw + 4, 0xFFFE);
    wr16(bw + 6, 0x0100);
    wr32(bw + 8, 0x12345678);  // CRC32
    wr16(bw + 0xC, 0);         // flags
    wr16(bw + 0xE, 1);         // num channels = 1

    // Channel Info at bw + 0x10
    uint8_t *ch = bw + 0x10;
    wr16(ch + 0, 1);
    wr16(ch + 2, 2);           // center pan
    wr32(ch + 4, 32000);       // sample rate
    wr32(ch + 0xC, 32);        // num samples = 32
    wr32(ch + 0x30, samples_off); // data start offset

    // Raw samples at samples_off
    memset(buf + samples_off, 0xAB, 16);

    return 0x200;
}

int main(void) {
    printf("[test_shrine_sensor] starting tests...\n");

    // 1. Invalid data
    assert(!shrine_sensor_patch_bars(NULL, 0, 50));
    uint8_t junk[32] = {0};
    assert(!shrine_sensor_patch_bars(junk, sizeof junk, 50));

    // 2. Build synthetic BARS
    uint8_t bars[1024];
    float orig_dummy_db = -1.5f;
    float orig_sensor_db = -3.0f;
    size_t bars_len = build_synthetic_bars(bars, sizeof bars, orig_dummy_db, orig_sensor_db);
    assert(bars_len == 0x200);

    // Verify initial values
    float cur_dummy = rdfloat(bars + 0x40 + 0x20 + 0x24);
    float cur_sensor = rdfloat(bars + 0xC0 + 0x20 + 0x24);
    assert(cur_dummy == orig_dummy_db);
    assert(cur_sensor == orig_sensor_db);
    assert(bars[0x1B0] == 0xAB);

    // 3. Patch with 100% volume (normal, no change)
    bool ok = shrine_sensor_patch_bars(bars, bars_len, 100);
    assert(ok);
    cur_dummy = rdfloat(bars + 0x40 + 0x20 + 0x24);
    cur_sensor = rdfloat(bars + 0xC0 + 0x20 + 0x24);
    assert(cur_dummy == orig_dummy_db);
    assert(cur_sensor == orig_sensor_db);
    assert(bars[0x1B0] == 0xAB);
    printf("  [pass] 100%% volume keeps original levels\n");

    // 4. Patch with 50% volume (-6.02 dB)
    ok = shrine_sensor_patch_bars(bars, bars_len, 50);
    assert(ok);
    cur_dummy = rdfloat(bars + 0x40 + 0x20 + 0x24);
    cur_sensor = rdfloat(bars + 0xC0 + 0x20 + 0x24);
    assert(cur_dummy == orig_dummy_db); // Dummy sound untouched
    float expected_50 = orig_sensor_db + 20.0f * log10f(0.5f);
    assert(fabsf(cur_sensor - expected_50) < 0.001f);
    assert(bars[0x1B0] == 0xAB); // Samples not zeroed
    printf("  [pass] 50%% volume attenuates sensor by %.2f dB\n", cur_sensor - orig_sensor_db);

    // 5. Patch with 0% volume (Mute)
    ok = shrine_sensor_patch_bars(bars, bars_len, 0);
    assert(ok);
    cur_dummy = rdfloat(bars + 0x40 + 0x20 + 0x24);
    cur_sensor = rdfloat(bars + 0xC0 + 0x20 + 0x24);
    assert(cur_dummy == orig_dummy_db);
    assert(cur_sensor == -999.0f);
    // Verify samples were zeroed
    for (int i = 0; i < 16; i++) {
        assert(bars[0x1B0 + i] == 0);
    }
    printf("  [pass] 0%% volume sets dB to -999.0 and zeroes audio samples\n");

    printf("[test_shrine_sensor] all tests passed successfully!\n");
    return 0;
}

