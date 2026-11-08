// The Steam Deck's own motion sensors, read from its controller's HID node (Linux). No SDL, threads or blocking I/O.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsu.h"

#define DECK_IMU_PERIOD_NS 4000000ull  // one state report every 4 ms (250 Hz)

typedef struct {
    int fd;
    bool fresh, have_num, failed;
    uint32_t num;       // the last report's packet number
    uint64_t received;  // host time of the last report
    uint64_t retry;     // next time to look for the device
} DeckImu;

// One 64-byte state report -> fusion axes (as DsuPacket: gravity in G, rad/s), its packet number in *num.
bool deck_imu_decode(const uint8_t *p, size_t n, DsuPacket *out, uint32_t *num);
void deck_imu_init(DeckImu *d);
void deck_imu_close(DeckImu *d);
// Reads the queued reports and hands each to sample (timestamp: microseconds from the packet numbers).
void deck_imu_poll(DeckImu *d, uint64_t now, DsuSampleFn sample, void *user);
