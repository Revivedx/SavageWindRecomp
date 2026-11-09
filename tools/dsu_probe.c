// Standalone DSU diagnostic: no SDL or game files. cc -Iruntime tools/dsu_probe.c runtime/dsu.c -lm -o dsu_probe
#define _POSIX_C_SOURCE 200809L
#include "dsu.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static unsigned samples;
static DsuPacket last;
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static void sample(const DsuPacket *p, uint32_t generation, void *user) {
    (void)generation; (void)user;
    last = *p;
    samples++;
}
int main(void) {
    DsuClient c;
    if (!dsu_init(&c, getenv("SAVAGEWIND_DSU_ENDPOINT"), (uint32_t)now_ns())) return 2;
    uint64_t end = now_ns() + 3000000000ull;
    while (now_ns() < end) {
        dsu_poll(&c, now_ns(), sample, NULL);
        nanosleep(&(struct timespec){.tv_nsec = 5000000}, NULL);
    }
    printf("DSU: %u samples, %s; gravity G = %.3f %.3f %.3f; gyro rad/s = %.3f %.3f %.3f\n",
           samples, c.fresh ? "fresh" : "unavailable", last.acc[0], last.acc[1], last.acc[2],
           last.gyro[0], last.gyro[1], last.gyro[2]);
    bool ok = samples && c.fresh;
    dsu_close(&c);
    return ok ? 0 : 1;
}
