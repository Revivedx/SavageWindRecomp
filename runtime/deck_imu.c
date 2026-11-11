// In Gaming mode Steam Input takes the Deck's controller and hands the game a virtual gamepad without motion sensors
// (#136), so SDL sees no gyroscope. The controller (28de:1205, interface 2) still sends its state report to every reader
// of its hidraw node, 250 times a second, motion included; Steam keeps reading it too. Report layout and scale cross-
// checked with SDL's HIDAPI Steam Deck driver (zlib) and kmicki/SteamDeckGyroDSU (MIT); an independent implementation.
#include "deck_imu.h"
#include <stdio.h>
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#endif

static int16_t le16s(const uint8_t *p) { return (int16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool deck_imu_decode(const uint8_t *p, size_t n, DsuPacket *out, uint32_t *num) {
    // Header: report 1, type 9 (the Deck's state), 64 bytes. Then the packet number, buttons and pads; the accelerometer
    // at 24 and the gyroscope at 30 (x, y, z, signed 16-bit: 2 G and 2000 degrees per second at full scale).
    if (n < 36 || p[0] != 1 || p[2] != 9 || p[3] != 64) return false;
    double a[3], w[3];
    for (int i = 0; i < 3; i++) a[i] = le16s(p + 24 + 2 * i) / 16384.0, w[i] = le16s(p + 30 + 2 * i);
    if (a[0] * a[0] + a[1] * a[1] + a[2] * a[2] < 0.01) return false;  // (no motion in the report)
    // SDL's axes are (x, z, -y), then motion_sensor_event's SDL -> Switch mapping: gravity (-x, -y, -z), turn (x, y, z).
    // Flat on a table the Deck reads +1 G on z: (0, 0, -1), as at rest elsewhere.
    const double rad = 2000.0 / 32768 * 3.14159265358979323846 / 180;
    DsuPacket v = {.type = 0x100002, .connected = true};
    for (int i = 0; i < 3; i++) v.acc[i] = -a[i], v.gyro[i] = w[i] * rad;
    *out = v;
    *num = le32(p + 4);
    return true;
}

void deck_imu_init(DeckImu *d) {
    memset(d, 0, sizeof *d);
    d->fd = -1;
}

void deck_imu_close(DeckImu *d) {
#ifdef __linux__
    if (d->fd >= 0) close(d->fd);
#endif
    d->fd = -1;
    d->fresh = d->have_num = false;
}

#ifdef __linux__
// The hidraw node of the controller's interface 2 (the one with the state reports), or -1.
static int open_deck(char *name, size_t size) {
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir) return -1;
    int fd = -1;
    for (struct dirent *e; fd < 0 && (e = readdir(dir));) {
        if (strncmp(e->d_name, "hidraw", 6)) continue;
        char path[512], link[PATH_MAX], line[256];
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        bool deck = false;
        while (fgets(line, sizeof line, f)) deck |= !strncmp(line, "HID_ID=0003:000028DE:00001205", 29);
        fclose(f);
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device", e->d_name);
        if (!deck || !realpath(path, link)) continue;  // (the link itself is relative: "../../../0003:28DE:1205.0005")
        char *dev = strrchr(link, '/');  // ".../3-3:1.2/0003:28DE:1205.0005": the interface before it
        if (!dev || dev - link < 4 || memcmp(dev - 4, ":1.2", 4)) continue;
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) snprintf(name, size, "%s", path);
        else fprintf(stderr, "[motion] Steam Deck: cannot open %s: %s\n", path, strerror(errno));
    }
    closedir(dir);
    return fd;
}
#endif

void deck_imu_poll(DeckImu *d, uint64_t now, DsuSampleFn sample, void *user) {
#ifdef __linux__
    if (d->fd < 0) {
        if (d->retry && now < d->retry) return;
        d->retry = now + 2 * DSU_RETRY_NS;
        char name[64];
        d->fd = open_deck(name, sizeof name);
        if (d->fd < 0) {
            if (!d->failed) fprintf(stderr, "[motion] Steam Deck: no controller HID node to read its gyroscope from\n");
            d->failed = true;
            return;
        }
        d->failed = false;
        fprintf(stderr, "[motion] Steam Deck: gyroscope and accelerometer read from %s (Steam Input's gamepad has none)\n",
                name);
    }
    if (d->fresh && now - d->received >= DSU_TIMEOUT_NS) d->fresh = d->have_num = false;
    // Bounded work: a frame's worth of reports and some (8 a frame at 30 fps).
    for (int i = 0; i < 256; i++) {
        uint8_t p[64];
        ssize_t n = read(d->fd, p, sizeof p);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                fprintf(stderr, "[motion] Steam Deck: reading the controller failed (%s); trying again\n", strerror(errno));
                deck_imu_close(d);
            }
            break;
        }
        DsuPacket v;
        uint32_t num;
        if (!deck_imu_decode(p, n, &v, &num)) continue;
        if (d->have_num && num == d->num) continue;
        d->num = num, d->have_num = true;
        d->received = now, d->fresh = true;
        // The packet numbers count 4 ms steps: the sensor's own clock, steady however late the frame reads them (a
        // step back, after 198 days of packets, is a gap the fusion skips).
        v.timestamp = (uint64_t)num * (DECK_IMU_PERIOD_NS / 1000);
        if (sample) sample(&v, 0, user);
    }
#else
    (void)d; (void)now; (void)sample; (void)user;
#endif
}
