// Small Linux Cemuhook client. No SDL, threads, DNS or blocking I/O.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DSU_TIMEOUT_NS 500000000ull
#define DSU_RETRY_NS 1000000000ull

typedef struct {
    uint32_t type, server, sequence;
    uint8_t slot, state, model, mac[6];
    bool connected;
    uint64_t timestamp;  // server microseconds
    double acc[3], gyro[3];  // existing motion fusion axes: gravity in G, angular velocity in rad/s
} DsuPacket;

typedef struct {
    int fd, slot;
    bool enabled, fresh, have_sequence;
    uint32_t client, server, sequence, generation;
    uint8_t mac[6];
    uint64_t received, timestamp, retry, invalid_log;
    uint32_t address;
    uint16_t port;
} DsuClient;

typedef void (*DsuSampleFn)(const DsuPacket *, uint32_t generation, void *);
// Packet helpers are independent of sockets and host endianness (also used by tests).
uint32_t dsu_crc(const uint8_t *p, size_t n);
size_t dsu_request(uint8_t out[28], uint32_t client, uint32_t type, int slot);
bool dsu_decode(const uint8_t *p, size_t n, DsuPacket *out);
void dsu_expire(DsuClient *c, uint64_t now);
bool dsu_accept(DsuClient *c, const DsuPacket *p, uint64_t now);
bool dsu_init(DsuClient *c, const char *endpoint, uint32_t client);
void dsu_close(DsuClient *c);
void dsu_poll(DsuClient *c, uint64_t now, DsuSampleFn sample, void *user);
