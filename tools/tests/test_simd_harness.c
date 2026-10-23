// Runs lifted FP/SIMD tests (tools/tests/test_simd.py) on the host; portable (Windows / POSIX).
// argv: code.bin buf.bin cases.bin out.bin. Prints nothing on success.
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "cpu.h"

extern GuestFn tests[];
extern int ntests;

#define BUF 0x7200000000ull
#define BUF_SIZE 0x20000
#define CODE 0x7210000000ull

void rt_dispatch(Cpu *c) { (void)c; }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; abort(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }
uint64_t rt_ticks(void) { return 0; }
volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }

static void *map_at(uint64_t addr, size_t len) {
#ifdef _WIN32
    void *p = VirtualAlloc((void *)addr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
#endif
    if (p != (void *)addr) {
        fprintf(stderr, "map %llx failed\n", (unsigned long long)addr);
        exit(1);
    }
    return p;
}

static size_t load(const char *path, void *dst, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    size_t n = fread(dst, 1, max, f);
    fclose(f);
    return n;
}

static uint32_t crc_tab[256];
static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

struct In { uint64_t k, fpcr, x[32], sp, nzcv; u128 v[32]; uint64_t memchk, pad; };
struct Out { uint64_t x[31], sp, nzcv, mem; u128 v[32]; };

int main(int argc, char **argv) {
    if (argc < 5) return 2;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        crc_tab[i] = c;
    }
    map_at(CODE, 0x400000);
    load(argv[1], (void *)CODE, 0x400000);
    uint8_t *buf = map_at(BUF, BUF_SIZE);
    uint8_t *pristine = malloc(BUF_SIZE);
    load(argv[2], pristine, BUF_SIZE);
    memcpy(buf, pristine, BUF_SIZE);
    FILE *f = fopen(argv[3], "rb"), *o = fopen(argv[4], "wb");
    if (!f || !o) return 1;
    static struct In in;
    static struct Out out;
    static Cpu c;
    while (fread(&in, sizeof in, 1, f) == 1) {
        memset(&c, 0, sizeof c);
        memcpy(c.x, in.x, sizeof c.x);
        c.sp = in.sp;
        c.nzcv = in.nzcv;
        c.fpcr = in.fpcr;
        memcpy(c.v, in.v, sizeof c.v);
        c.excl_addr = ~0ull;
        tests[in.k](&c);
        memcpy(out.x, c.x, sizeof out.x);
        out.sp = c.sp;
        out.nzcv = c.nzcv & 0xf0000000ull;
        out.mem = 0;
        if (in.memchk) {
            out.mem = crc32_buf(buf, BUF_SIZE);
            memcpy(buf, pristine, BUF_SIZE);
        }
        memcpy(out.v, c.v, sizeof out.v);
        fwrite(&out, sizeof out, 1, o);
    }
    fclose(o);
    return 0;
}
