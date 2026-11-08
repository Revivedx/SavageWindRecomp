// A folder to attach to a bug report (runtime/problem_report.h), in the data folder (the working directory,
// runtime/paths.h). The logs are where the platform writes them (plat_log_path: next to the program on Windows), the
// settings file is the one in use (settings_file).
#include "problem_report.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "gamelib.h"
#include "hle.h"
#include "paths.h"
#include "platform.h"
#include "settings.h"
#include <string.h>

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
    int copied = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        struct stat st;
        snprintf(to, sizeof to, "%s/%s", dir, names[i]);
        if (from[i][0] && stat(from[i], &st) == 0 && SDL_CopyFile(from[i], to)) copied++;
    }
    snprintf(to, sizeof to, "%s/system.txt", dir);
    FILE *f = fopen(to, "w");
    if (f) {
        fprintf(f, "Savage Wind %s\n", rt_version());  // (the build it came from: the log's first line)
        fprintf(f, "System: %s, %d logical processors, %d MB of memory\n", SDL_GetPlatform(), SDL_GetNumLogicalCPUCores(),
                SDL_GetSystemRAM());
        fprintf(f, "Game code: %s%s\n", gamelib_id(), gamelib_compat() ? " (compatible mode)" : "");
        fprintf(f, "Made: %s\n", when);
        if (!log[0]) fprintf(f, "Log: not in a file (this run's output went to a console or was redirected)\n");
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
