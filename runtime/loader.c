// NSO loading, relocation, and symbol resolution.
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "rt.h"

Module g_modules[MAX_MODULES];
int g_nmodules;

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28,
       DT_RELRSZ = 35, DT_RELR = 36, DT_GNU_HASH = 0x6FFFFEF5 };
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64Sym;
typedef struct { uint64_t off, info; int64_t addend; } Elf64Rela;

static size_t lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    const uint8_t *s = src, *se = src + srclen;
    uint8_t *d = dst, *de = dst + dstlen;
    while (s < se) {
        uint8_t tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { b = *s++; lit += b; } while (b == 255); }
        if (d + lit > de || s + lit > se) break;
        memcpy(d, s, lit);
        d += lit; s += lit;
        if (s >= se) break;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { b = *s++; ml += b; } while (b == 255); }
        ml += 4;
        uint8_t *m = d - off;
        if (m < dst || d + ml > de) break;
        while (ml--) *d++ = *m++;
    }
    return d - dst;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    fread(p, 1, *len, f);
    fclose(f);
    return p;
}

// Number of dynsym entries from a DT_GNU_HASH table: the end of its last chain. Newer SDK modules (sdk and subsdk0
// of BOTW 1.8.2) have no DT_HASH, whose nchain says it directly.
static uint64_t gnu_hash_nsyms(uint64_t h) {
    const uint32_t *w = (const uint32_t *)h;
    uint32_t nbuckets = w[0], symoffset = w[1], bloom_size = w[2];
    const uint32_t *buckets = (const uint32_t *)(h + 16 + 8ull * bloom_size), *chain = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++)
        if (buckets[i] > last) last = buckets[i];
    if (last < symoffset) return symoffset;  // (no hashed symbols)
    while (!(chain[last - symoffset] & 1)) last++;
    return (uint64_t)last + 1;
}

// Loads the module where the game code expects it (lm: its entry in the library's layout). The file must be the one
// the code was generated from: same build id, and so the same image size.
static void load_nso(Module *m, const char *path, const GameLibModule *lm) {
    size_t len;
    uint8_t *raw = read_file(path, &len);
    if (!raw || len < 0x100 || memcmp(raw, "NSO0", 4)) FATAL("cannot load %s\n", path);
    char id[65];
    for (int i = 0; i < 32; i++) snprintf(id + 2 * i, 3, "%02x", raw[0x40 + i]);
    if (strcmp(id, lm->build_id))
        FATAL("%s is not the file the game code was built from (build %.16s, the code's %.16s): build it again\n",
              path, id, lm->build_id);
    uint64_t base = lm->base, size = lm->size;
    if (base < MAIN_BASE || base + size > MODULES_END || size > MODULES_END)
        FATAL("%s: the game code puts it at %llx-%llx, outside the module area %llx-%llx\n", m->name, base,
              base + size, MAIN_BASE, MODULES_END);
    uint32_t flags = *(uint32_t *)(raw + 0xC);
    uint32_t seg[3][3];  // file_off, mem_off, size
    for (int i = 0; i < 3; i++) memcpy(seg[i], raw + 0x10 + i * 0x10, 12);
    uint32_t bss = *(uint32_t *)(raw + 0x3C);
    for (int i = 0; i < 3; i++)
        if ((uint64_t)seg[i][1] + seg[i][2] + (i == 2 ? bss : 0) > size)
            FATAL("%s: segment %d past its image\n", path, i);
    plat_commit(base, size);  // inside the guest window: zero-filled, read/write
    for (int i = 0; i < 3; i++) {
        uint32_t csize = *(uint32_t *)(raw + 0x60 + i * 4);
        uint8_t *dst = (uint8_t *)base + seg[i][1];
        if (flags & (1 << i)) {
            size_t got = lz4_decompress(raw + seg[i][0], csize, dst, seg[i][2]);
            if (got != seg[i][2]) FATAL("lz4 %s seg %d: %zu != %u\n", path, i, got, seg[i][2]);
        } else {
            memcpy(dst, raw + seg[i][0], seg[i][2]);
        }
    }
    free(raw);
    m->base = base;
    m->size = size;
    m->text_off = seg[0][1]; m->text_size = seg[0][2];
    m->ro_off = seg[1][1];   m->ro_size = seg[1][2];
    m->data_off = seg[2][1]; m->data_size = seg[2][2] + bss;

    uint32_t mod0 = *(uint32_t *)(base + 4);
    uint64_t mod = base + mod0;
    m->dynamic = mod + *(int32_t *)(mod + 4);
    uint64_t gnu_hash = 0;
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        switch (d[0]) {
        case DT_SYMTAB: m->symtab = base + d[1]; break;
        case DT_STRTAB: m->strtab = base + d[1]; break;
        case DT_HASH: m->nsyms = *(uint32_t *)(base + d[1] + 4); break;
        case DT_GNU_HASH: gnu_hash = base + d[1]; break;
        case 12: m->init = base + d[1]; break;  // DT_INIT
        case 13: m->fini = base + d[1]; break;  // DT_FINI
        case DT_INIT_ARRAY: m->init_array = base + d[1]; break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d[1]; break;
        case DT_FINI_ARRAY: m->fini_array = base + d[1]; break;
        case DT_FINI_ARRAYSZ: m->fini_array_sz = d[1]; break;
        }
    }
    if (!m->nsyms && gnu_hash) m->nsyms = gnu_hash_nsyms(gnu_hash);
    LOG("loaded %-8s at %llx size %llx syms %llu\n", m->name, base, size, m->nsyms);
}

Module *loader_module(const char *name) {
    for (int i = 0; i < g_nmodules; i++)
        if (!strcmp(g_modules[i].name, name)) return &g_modules[i];
    return NULL;
}

static Elf64Sym *sym(Module *m, uint64_t i) { return (Elf64Sym *)m->symtab + i; }
static const char *symname(Module *m, Elf64Sym *s) { return (const char *)(m->strtab + s->name); }

uint64_t loader_find_symbol_in(const char *module, const char *name) {
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        if (strcmp(m->name, module)) continue;
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
        }
    }
    return 0;
}

uint64_t loader_find_symbol(const char *name) {
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
        }
    }
    return 0;
}

const char *loader_symbol_at(uint64_t addr, uint64_t *offset) {
    const char *best = NULL;
    uint64_t best_addr = 0;
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        if (addr < m->base || addr >= m->base + m->size) continue;
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            uint64_t a = m->base + s->value;
            if (s->shndx && (s->info & 0xF) == 2 && a <= addr && a > best_addr) {
                best = symname(m, s);
                best_addr = a;
            }
        }
    }
    if (offset) *offset = addr - best_addr;
    return best;
}

static uint64_t resolve(Module *m, uint32_t symidx) {
    Elf64Sym *s = sym(m, symidx);
    const char *name = symname(m, s);
    if (s->shndx) return m->base + s->value;
    HostFn impl = hle_lookup_impl(name);
    if (impl) return hle_register(name, impl);  // host implementation wins over the guest SDK's
    uint64_t a = loader_find_symbol(name);
    if (a) return a;
    if ((s->info >> 4) == 2) return 0;  // weak undefined
    return hle_register(name, NULL);
}

// DT_RELR: packed relative relocations (newer SDKs: every module of BOTW 1.8.2 keeps its R_RELATIVE ones only here).
// An even entry is the offset of a word to relocate; an odd one is a bitmap of the 63 words after the last one
// relocated. The addend is the word itself. Returns how many words were relocated.
static uint64_t relocate_relr(Module *m, uint64_t relr, uint64_t size) {
    uint64_t *where = NULL, n = 0;
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t e = ((uint64_t *)relr)[i];
        if (!(e & 1)) {
            where = (uint64_t *)(m->base + e);
            *where++ += m->base;
            n++;
            continue;
        }
        if (!where) FATAL("%s: DT_RELR bitmap before any address\n", m->name);
        for (int b = 0; b < 63; b++)
            if ((e >> (b + 1)) & 1) where[b] += m->base, n++;
        where += 63;
    }
    return n;
}

static void relocate(Module *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        if (d[0] == DT_RELA) rela = m->base + d[1];
        if (d[0] == DT_RELASZ) relasz = d[1];
        if (d[0] == DT_JMPREL) jmprel = m->base + d[1];
        if (d[0] == DT_PLTRELSZ) pltsz = d[1];
        if (d[0] == DT_RELR) relr = m->base + d[1];
        if (d[0] == DT_RELRSZ) relrsz = d[1];
    }
    uint64_t packed = relr ? relocate_relr(m, relr, relrsz) : 0;
    int unresolved = 0;
    for (int pass = 0; pass < 2; pass++) {
        Elf64Rela *r = (Elf64Rela *)(pass ? jmprel : rela);
        uint64_t n = (pass ? pltsz : relasz) / sizeof(Elf64Rela);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t *where = (uint64_t *)(m->base + r[i].off);
            uint32_t type = (uint32_t)r[i].info, si = r[i].info >> 32;
            switch (type) {
            case R_RELATIVE: *where = m->base + r[i].addend; break;
            case R_ABS64:
            case R_GLOB_DAT:
            case R_JUMP_SLOT: {
                uint64_t s = resolve(m, si);
                *where = s ? s + r[i].addend : 0;
                if (s >= HLE_BASE && s < HLE_BASE + HLE_SIZE) unresolved++;
                break;
            }
            default: FATAL("%s: reloc type %u\n", m->name, type);
            }
        }
    }
    LOG("relocated %-8s (%llu packed relative, %d imports bound to host stubs)\n", m->name,
        (unsigned long long)packed, unresolved);
}

// The modules the game code was generated for, in its layout's order (tools/recomp/layout.py: main, subsdk0..9, sdk).
void loader_load(const char *exefs) {
    char path[1024];
    const GameLibTables *g = gamelib_tables();
    if (*g->nmodules > MAX_MODULES) FATAL("the game code has %u modules\n", *g->nmodules);
    for (uint32_t i = 0; i < *g->nmodules; i++) {
        Module *m = &g_modules[g_nmodules++];
        memset(m, 0, sizeof *m);
        m->name = g->modules[i].name;
        snprintf(path, sizeof path, "%s/%s", exefs, m->name);
        load_nso(m, path, &g->modules[i]);
    }
    for (int i = 0; i < 10; i++) {  // (a subsdk the code doesn't have: other game files than it was built from)
        char name[16];
        snprintf(name, sizeof name, "subsdk%d", i);
        snprintf(path, sizeof path, "%s/%s", exefs, name);
        FILE *f = loader_module(name) ? NULL : fopen(path, "rb");
        if (f) { fclose(f); FATAL("%s is not in the game code: build it again\n", path); }
    }
    for (int i = 0; i < g_nmodules; i++) relocate(&g_modules[i]);
}
