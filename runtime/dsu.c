// Protocol reference: https://v1993.github.io/cemuhook-protocol/
// Axis cross-check: kmicki/SteamDeckGyroDSU (MIT), cemuhookadapter.cpp / sdhidframe.h,
// and SDL's HIDAPI Steam Deck driver (zlib). This is an independent implementation.
#include "dsu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static float lefloat(const uint8_t *p) {
    uint32_t bits = le32(p);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

uint32_t dsu_crc(const uint8_t *p, size_t n) {
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; i++) {
        crc ^= i >= 8 && i < 12 ? 0 : p[i];
        for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

size_t dsu_request(uint8_t out[28], uint32_t client, uint32_t type, int slot) {
    size_t n = type == 0x100000 ? 20 : 28;
    memset(out, 0, 28);
    memcpy(out, "DSUC", 4);
    out[4] = 0xe9, out[5] = 3;  // protocol 1001
    out[6] = n - 16;
    put32(out + 12, client);
    put32(out + 16, type);
    if (type == 0x100001) {
        put32(out + 20, 4);
        for (int i = 0; i < 4; i++) out[24 + i] = i;
    } else if (type == 0x100002) {
        out[20] = 1, out[21] = slot;  // subscribe to exactly one slot
    }
    put32(out + 8, dsu_crc(out, n));
    return n;
}

bool dsu_decode(const uint8_t *p, size_t n, DsuPacket *out) {
    if (n < 20 || memcmp(p, "DSUS", 4) || le16(p + 4) != 1001) return false;
    size_t size = 16u + le16(p + 6);
    if (size < 20 || size > n) return false;
    // SteamDeckGyroDSU 2.1: VersionData has two C++ padding bytes, CRC covers all 24 bytes,
    // and PrepareAnswerConstants leaves eventType at zero. Accept only that exact legacy reply.
    bool deck_version = size == 22 && n == 24 && le32(p + 16) == 0 &&
                        le16(p + 20) == 1001 && !p[22] && !p[23] && le32(p + 8) == dsu_crc(p, 24);
    if (!deck_version && le32(p + 8) != dsu_crc(p, size)) return false;
    DsuPacket v = {.type = deck_version ? 0x100000 : le32(p + 16), .server = le32(p + 12)};
    if (v.type == 0x100000) {
        if (size != 22 || le16(p + 20) < 1001) return false;
    } else if (v.type == 0x100001 || v.type == 0x100002) {
        if (size != (v.type == 0x100001 ? 32 : 100) || p[20] > 3 || p[21] > 2) return false;
        v.slot = p[20], v.state = p[21], v.model = p[22];
        memcpy(v.mac, p + 24, 6);
        if (v.type == 0x100002) {
            v.connected = p[31] == 1 && v.state == 2;
            v.sequence = le32(p + 32);
            v.timestamp = le64(p + 68);
            double a[3], w[3];
            for (int i = 0; i < 3; i++) {
                a[i] = lefloat(p + 76 + 4 * i);
                w[i] = lefloat(p + 88 + 4 * i);
                if (!isfinite(a[i]) || !isfinite(w[i]) || fabs(a[i]) > 32 || fabs(w[i]) > 10000) return false;
            }
            // DSU -> SDL: acceleration = (-X,-Y,-Z)*gravity; gyro = (pitch,-yaw,-roll)*pi/180.
            // Then the same SDL -> Switch mapping as motion_sensor_event. Flat DSU: (0,-1,0).
            const double rad = 3.14159265358979323846 / 180;
            v.acc[0] = a[0], v.acc[1] = -a[2], v.acc[2] = a[1];
            v.gyro[0] = w[0] * rad, v.gyro[1] = w[2] * rad, v.gyro[2] = -w[1] * rad;
        }
    } else return false;
    *out = v;
    return true;
}

static void forget(DsuClient *c) {
    c->fresh = c->have_sequence = false;
    c->slot = -1;
    c->generation++;
}

void dsu_expire(DsuClient *c, uint64_t now) {
    if (c->have_sequence && now - c->received >= DSU_TIMEOUT_NS) {
        fprintf(stderr, "[dsu] disconnected: motion timeout; retrying\n");
        forget(c);
        c->retry = 0;
    }
}

bool dsu_accept(DsuClient *c, const DsuPacket *p, uint64_t now) {
    if (p->type != 0x100002 || c->slot != p->slot || c->server != p->server || memcmp(c->mac, p->mac, 6)) return false;
    if (!p->connected) { forget(c); c->retry = 0; return false; }
    if (c->have_sequence) {
        uint32_t delta = p->sequence - c->sequence;
        if (!delta || delta >= 0x80000000u || p->timestamp <= c->timestamp) return false;
    }
    double an = p->acc[0]*p->acc[0] + p->acc[1]*p->acc[1] + p->acc[2]*p->acc[2];
    if (an < 0.01) return false;  // uninitialized/all-zero motion is not a usable sensor
    if (!c->fresh) fprintf(stderr, "[dsu] connected: slot %d, server %08x\n", c->slot, c->server);
    c->fresh = c->have_sequence = true;
    c->sequence = p->sequence, c->timestamp = p->timestamp, c->received = now;
    return true;
}

bool dsu_init(DsuClient *c, const char *endpoint, uint32_t client) {
    *c = (DsuClient){.fd = -1, .slot = -1, .client = client, .port = 26760};
#ifdef __linux__
    char host[64] = "127.0.0.1";
    if (endpoint) {
        const char *colon = strchr(endpoint, ':');
        if (!colon || colon == endpoint || (size_t)(colon - endpoint) >= sizeof host) goto bad;
        memcpy(host, endpoint, colon - endpoint);
        host[colon - endpoint] = 0;
        char *end;
        long port = strtol(colon + 1, &end, 10);
        if (!colon[1] || *end || port < 1 || port > 65535) goto bad;
        c->port = port;
    }
    if (inet_pton(AF_INET, host, &c->address) != 1) goto bad;
    c->enabled = true;
    fprintf(stderr, "[dsu] enabled: %s:%u (nonblocking)\n", host, c->port);
    return true;
bad:
    fprintf(stderr, "[dsu] invalid SAVAGEWIND_DSU_ENDPOINT; use numeric IPv4:port\n");
#else
    (void)endpoint;
#endif
    return false;
}

void dsu_close(DsuClient *c) {
#ifdef __linux__
    if (c->fd >= 0) close(c->fd);
#endif
    c->fd = -1;
    forget(c);
}

#ifdef __linux__
static void request(DsuClient *c, uint32_t type) {
    uint8_t p[28];
    size_t n = dsu_request(p, c->client, type, c->slot);
    // Transient errors (including ICMP port unreachable) are retried at the next heartbeat.
    (void)send(c->fd, p, n, MSG_DONTWAIT);
}
#endif

void dsu_poll(DsuClient *c, uint64_t now, DsuSampleFn sample, void *user) {
#ifdef __linux__
    if (!c->enabled) return;
    dsu_expire(c, now);
    if (!c->retry || now >= c->retry) {
        c->retry = now + DSU_RETRY_NS;
        if (c->fd < 0) {
            c->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (c->fd < 0) return;
            struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(c->port), .sin_addr.s_addr = c->address};
            if (connect(c->fd, (struct sockaddr *)&addr, sizeof addr)) { dsu_close(c); return; }
        }
        request(c, 0x100000);
        request(c, 0x100001);
        if (c->slot >= 0) request(c, 0x100002);
    }
    // Bounded work even if a server floods the socket. Process each queued motion sample for fusion.
    for (int i = 0; i < 128 && c->fd >= 0; i++) {
        uint8_t data[2048];
        ssize_t n = recv(c->fd, data, sizeof data, MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNREFUSED && errno != EINTR) dsu_close(c);
            break;
        }
        DsuPacket p;
        if ((size_t)n > sizeof data || !dsu_decode(data, n, &p)) {
            if (!c->invalid_log || now >= c->invalid_log) {
                fprintf(stderr, "[dsu] invalid packet ignored (header/length/CRC/motion)\n");
                c->invalid_log = now + 5000000000ull;
            }
            continue;
        }
        if (p.type == 0x100001) {
            if (c->slot >= 0 && p.server != c->server) { forget(c); c->retry = 0; }
            if (p.slot == c->slot && (p.state != 2 || memcmp(c->mac, p.mac, 6))) { forget(c); c->retry = 0; }
            if (c->slot < 0 && p.state == 2 && p.model == 2) {
                c->slot = p.slot, c->server = p.server;
                memcpy(c->mac, p.mac, 6);
                c->generation++;
                request(c, 0x100002);
            }
        } else if (dsu_accept(c, &p, now) && sample) sample(&p, c->generation, user);
    }
#else
    (void)c; (void)now; (void)sample; (void)user;
#endif
}
