// A folder to attach to a bug report (runtime/problem_report.h). The data folder is the working directory
// (runtime/paths.h), so the logs and the settings file are found by name.
#include "problem_report.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "gamelib.h"
#include "hle.h"
#include "paths.h"
#include "platform.h"

const char *rt_version(void);  // runtime/ui/launcher.mm

const char *problem_report_make(void) {
    static char dir[64];
    char when[32] = "0", to[128];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) strftime(when, sizeof when, "%Y%m%d-%H%M%S", tm);
    snprintf(dir, sizeof dir, "problem-report-%s", when);
    if (!SDL_CreateDirectory(dir)) {
        dir[0] = 0;
        return dir;
    }
    static const char *const files[] = {"savagewind.log", "savagewind.prev.log", "savagewind.ini"};
    int copied = 0;
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        struct stat st;
        snprintf(to, sizeof to, "%s/%s", dir, files[i]);
        if (stat(files[i], &st) == 0 && SDL_CopyFile(files[i], to)) copied++;
    }
    snprintf(to, sizeof to, "%s/system.txt", dir);
    FILE *f = fopen(to, "w");
    if (f) {
        fprintf(f, "Savage Wind %s\n", rt_version());  // (the build it came from: the log's first line)
        fprintf(f, "System: %s, %d logical processors, %d MB of memory\n", SDL_GetPlatform(), SDL_GetNumLogicalCPUCores(),
                SDL_GetSystemRAM());
        fprintf(f, "Game code: %s%s\n", gamelib_id(), gamelib_compat() ? " (compatible mode)" : "");
        fprintf(f, "Made: %s\n", when);
        fclose(f);
    }
    LOG("[report] %s: %d files and system.txt\n", dir, copied);
    return dir;
}

void problem_report_show(const char *dir) {
    if (!dir || !*dir) return;
    char path[1200], url[3700];
    snprintf(path, sizeof path, "%s/%s", rt_data_root(), dir);
    size_t n = snprintf(url, sizeof url, "file://%s", path[0] == '/' ? "" : "/");
    for (const char *c = path; *c && n + 4 < sizeof url; c++) {  // (a URL: spaces and the like escaped)
        unsigned char ch = (unsigned char)*c == '\\' ? '/' : (unsigned char)*c;
        if (ch == ' ' || ch == '%' || ch == '#') n += snprintf(url + n, sizeof url - n, "%%%02X", ch);
        else url[n++] = (char)ch;
    }
    url[n] = 0;
    SDL_OpenURL(url);
}
