// tools/test_update_appimage.py builds this: the AppImage update on its own, without the game.
// usage: test_update_appimage <check|apply|back> <work folder> <current version>; prints key=value lines.
#include "../runtime/update_appimage.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    UpdateProblem p = {{0}};
    UcRelease r;
    bool available = false, ok;
    if (!strcmp(argv[1], "back")) ok = appimage_go_back(&p);
    else {
        ok = appimage_find_update(argv[2], argv[3], &r, &available, &p);
        if (ok && available && !strcmp(argv[1], "apply")) {
            volatile float progress = 0;
            ok = appimage_install(argv[2], &r, &progress, &p);
            if (ok) {
                printf("status=installed\n");
                fflush(stdout);
                appimage_relaunch(&p);
                ok = false;
            }
        }
    }
    if (!ok) printf("status=error\ncode=%s\ntitle=%s\nwhat=%s\n", p.code, p.title, p.what);
    else if (!strcmp(argv[1], "back")) printf("status=back\n");
    else printf("status=%s\nversion=%s\n", available ? "available" : "current", available ? r.version : "");
    return !ok;
}
