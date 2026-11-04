// Runs lifted single-instruction tests (tools/test_lift.py); prints final state per case. Portable (Windows / POSIX).
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
#define BUF_SIZE 0x40000

void rt_dispatch(Cpu *c) { (void)c; }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; abort(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }
uint64_t rt_ticks(void) { return 0; }
volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }

static void *map_file(const char *path, uint64_t addr, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t len = (*size + 0x3fff) & ~0x3fffull;
#ifdef _WIN32
    // (reservations start on 64 KB boundaries: sdk's base is only page-aligned)
    uint64_t lo = addr & ~0xffffull;
    void *p = VirtualAlloc((void *)lo, len + (addr - lo), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p == (void *)lo) p = (void *)addr;
#else
    void *p = mmap((void *)addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
#endif
    if (p != (void *)addr) { fprintf(stderr, "map %llx failed\n", (unsigned long long)addr); exit(1); }
    if (fread(p, 1, *size, f) != *size) { perror(path); exit(1); }
    fclose(f);
    return p;
}

static uint32_t crc_tab[256];
static uint32_t crc32_buf(const uint8_t *p, size_t n) {  // (zlib's crc32, as test_lift.py hashes Unicorn's memory)
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

int main(int argc, char **argv) {
    if (argc < 5) return 2;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    uint64_t base = strtoull(argv[4], 0, 16);
    size_t isz, bsz;
    map_file(argv[1], base, &isz);
    map_file(argv[2], BUF, &bsz);
    uint8_t *pristine = malloc(BUF_SIZE);
    memcpy(pristine, (void *)BUF, BUF_SIZE);
    FILE *f = fopen(argv[3], "rb");
    if (!f) { perror(argv[3]); return 1; }
    struct { uint64_t k, pc, x[32], sp, nzcv; __uint128_t v[32]; } in;
    static Cpu c;
    while (fread(&in, sizeof in, 1, f) == 1) {
        memcpy((void *)BUF, pristine, BUF_SIZE);
        memset(&c, 0, sizeof c);
        memcpy(c.x, in.x, sizeof c.x);
        c.sp = in.sp;
        c.nzcv = in.nzcv;
        memcpy(c.v, in.v, sizeof c.v);
        c.excl_addr = ~0ull;
        tests[in.k](&c);
        for (int i = 0; i < 31; i++) printf("%llx ", (unsigned long long)c.x[i]);
        printf("%llx %llx %llx ", (unsigned long long)c.sp, (unsigned long long)(c.nzcv & 0xf0000000ull),
               (unsigned long long)c.pc);
        printf("%08x", crc32_buf((const uint8_t *)BUF, BUF_SIZE));
        for (int i = 0; i < 32; i++)
            printf(" %016llx%016llx", (unsigned long long)(uint64_t)(c.v[i] >> 64), (unsigned long long)(uint64_t)c.v[i]);
        printf("\n");
    }
    return 0;
}
