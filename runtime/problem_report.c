// A zip file to attach to a bug report (runtime/problem_report.h), in the data folder (the working directory,
// runtime/paths.h). The logs are where the platform writes them (plat_log_path: next to the program on Windows), the
// settings file is the one in use (settings_file). GitHub takes one zip, and neither a folder nor a .ini.
#include "problem_report.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <zlib.h>

#include "gamelib.h"
#include "hle.h"
#include "paths.h"
#include "platform.h"
#include "settings.h"
#include <string.h>

const char *rt_version(void);  // runtime/ui/launcher.mm

enum { REPORT_FILES = 4, REPORT_MAX_FILE = 0x40000000 };

typedef struct {
    const char *name;
    uint32_t crc, offset;
    uint32_t size, packed;
    int method;  // 8 deflated, 0 stored
} ZipEntry;

static unsigned char *read_whole(const char *path, size_t *size) {
    FILE *f = path[0] ? fopen(path, "rb") : NULL;  // (the running log is open for writing: fopen shares it)
    if (!f) return NULL;
    unsigned char *data = NULL;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (n >= 0 && n <= REPORT_MAX_FILE && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)n + 1)))
        *size = fread(data, 1, (size_t)n, f);
    fclose(f);
    return data;
}

// Raw deflate (no zlib header, as zip wants it). NULL if it fails.
static unsigned char *deflate_raw(const unsigned char *in, size_t n, size_t *packed) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    uLong cap = deflateBound(&z, (uLong)n);
    unsigned char *out = malloc(cap);
    int r = Z_STREAM_ERROR;
    if (out) {
        z.next_in = (Bytef *)in;
        z.avail_in = (uInt)n;
        z.next_out = out;
        z.avail_out = (uInt)cap;
        r = deflate(&z, Z_FINISH);
    }
    *packed = z.total_out;
    deflateEnd(&z);
    if (r != Z_STREAM_END) {
        free(out);
        return NULL;
    }
    return out;
}

static void put16(unsigned char **p, unsigned v) {
    *(*p)++ = (unsigned char)v;
    *(*p)++ = (unsigned char)(v >> 8);
}

static void put32(unsigned char **p, uint32_t v) {
    put16(p, v & 0xffff);
    put16(p, v >> 16);
}

// One file into the zip; the entry says where it went. False if the disk write failed.
static bool zip_add(FILE *zip, ZipEntry *e, const unsigned char *data, size_t size, unsigned dos_time, unsigned dos_date) {
    size_t packed = 0;
    unsigned char *deflated = deflate_raw(data, size, &packed);
    bool use = deflated && packed < size;
    unsigned char head[30], *p = head;
    size_t name_len = strlen(e->name);
    e->offset = (uint32_t)ftell(zip);
    e->crc = (uint32_t)crc32(0, data, (uInt)size);
    e->size = (uint32_t)size;
    e->packed = (uint32_t)(use ? packed : size);
    e->method = use ? 8 : 0;
    put32(&p, 0x04034b50);
    put16(&p, 20);  // version needed: 2.0
    put16(&p, 0);
    put16(&p, (unsigned)e->method);
    put16(&p, dos_time);
    put16(&p, dos_date);
    put32(&p, e->crc);
    put32(&p, e->packed);
    put32(&p, e->size);
    put16(&p, (unsigned)name_len);
    put16(&p, 0);
    bool ok = fwrite(head, 1, sizeof head, zip) == sizeof head && fwrite(e->name, 1, name_len, zip) == name_len &&
              fwrite(use ? deflated : data, 1, e->packed, zip) == e->packed;
    free(deflated);
    return ok;
}

// The central directory that ends a zip.
static bool zip_finish(FILE *zip, const ZipEntry *entries, int count, unsigned dos_time, unsigned dos_date) {
    uint32_t start = (uint32_t)ftell(zip);
    uint32_t dir_size = 0;
    bool ok = true;
    for (int i = 0; i < count; i++) {
        const ZipEntry *e = &entries[i];
        unsigned char head[46], *p = head;
        size_t name_len = strlen(e->name);
        put32(&p, 0x02014b50);
        put16(&p, 20);  // made by
        put16(&p, 20);  // version needed
        put16(&p, 0);
        put16(&p, (unsigned)e->method);
        put16(&p, dos_time);
        put16(&p, dos_date);
        put32(&p, e->crc);
        put32(&p, e->packed);
        put32(&p, e->size);
        put16(&p, (unsigned)name_len);
        put16(&p, 0);  // extra
        put16(&p, 0);  // comment
        put16(&p, 0);  // disk
        put16(&p, 0);  // internal attributes
        put32(&p, 0);  // external attributes
        put32(&p, e->offset);
        ok = ok && fwrite(head, 1, sizeof head, zip) == sizeof head && fwrite(e->name, 1, name_len, zip) == name_len;
        dir_size += (uint32_t)(sizeof head + name_len);
    }
    unsigned char end[22], *p = end;
    put32(&p, 0x06054b50);
    put16(&p, 0);
    put16(&p, 0);
    put16(&p, (unsigned)count);
    put16(&p, (unsigned)count);
    put32(&p, dir_size);
    put32(&p, start);
    put16(&p, 0);
    return ok && fwrite(end, 1, sizeof end, zip) == sizeof end;
}

const char *problem_report_make(void) {
    static char zip_name[64];
    char when[32] = "0";
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    unsigned dos_time = 0, dos_date = 0x21;  // (1980-01-01, zip's earliest)
    if (tm) {
        strftime(when, sizeof when, "%Y%m%d-%H%M%S", tm);
        dos_time = (unsigned)(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2);
        if (tm->tm_year >= 80) dos_date = (unsigned)((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday);
    }
    snprintf(zip_name, sizeof zip_name, "problem-report-%s.zip", when);
    FILE *zip = fopen(zip_name, "wb");
    if (!zip) {
        zip_name[0] = 0;
        return zip_name;
    }
    char log[1100] = "", prev[1100] = "";
    const char *lp = plat_log_path();
    if (lp && *lp) {
        snprintf(log, sizeof log, "%s", lp);
        snprintf(prev, sizeof prev, "%s", lp);
        char *name = strstr(prev, "savagewind.log");
        if (name) snprintf(name, sizeof prev - (size_t)(name - prev), "savagewind.prev.log");
        else prev[0] = 0;
    }
    const char *const from[] = {log, prev, settings_file()};
    static const char *const names[] = {"savagewind.log", "savagewind.prev.log", "savagewind.ini"};
    ZipEntry entries[REPORT_FILES];
    int count = 0;
    bool ok = true;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && ok; i++) {
        size_t size = 0;
        unsigned char *data = read_whole(from[i], &size);
        if (!data) continue;
        entries[count].name = names[i];
        ok = zip_add(zip, &entries[count], data, size, dos_time, dos_date);
        count++;
        free(data);
    }
    int copied = count;
    char summary[512];
    int n = snprintf(summary, sizeof summary,
                     "Savage Wind %s\n"  // (the build it came from: the log's first line)
                     "System: %s, %d logical processors, %d MB of memory\n"
                     "Game code: %s%s\n"
                     "Made: %s\n"
                     "%s",
                     rt_version(), SDL_GetPlatform(), SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM(), gamelib_id(),
                     gamelib_compat() ? " (compatible mode)" : "", when,
                     log[0] ? "" : "Log: not in a file (this run's output went to a console or was redirected)\n");
    if (ok && n > 0) {
        entries[count].name = "system.txt";
        ok = zip_add(zip, &entries[count], (const unsigned char *)summary, strlen(summary), dos_time, dos_date);
        count++;
    }
    ok = ok && zip_finish(zip, entries, count, dos_time, dos_date);
    if (fclose(zip) != 0) ok = false;
    if (!ok) {
        LOG("[report] couldn't write %s\n", zip_name);
        remove(zip_name);
        zip_name[0] = 0;
        return zip_name;
    }
    LOG("[report] %s: %d files and system.txt\n", zip_name, copied);
    return zip_name;
}
