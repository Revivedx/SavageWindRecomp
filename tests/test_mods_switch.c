// Turning mods on and off: their files between mods/ and mods/disabled/, in a folder of its own
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define MODS_TEST
#include "../runtime/mods.c"

static void make_file(const char *path) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs("x", f);
    fclose(f);
}

static bool exists(const char *path) {
    struct stat sb;
    return !stat(path, &sb);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!chdir(argv[1]));
    assert(mods_count() == 0);  // (no folder yet: no mods, and no error)
    mods_scan();
    mods_mkdir("mods");
    make_file("mods/Zelda Hair.zip");
    make_file("mods/link.bnp");
    make_file("mods/readme.txt");  // (not a mod)
    mods_scan();
    assert(mods_count() == 2);
    assert(!strcmp(mods_title(0), "link") && !strcmp(mods_title(1), "Zelda Hair"));  // (sorted, without the extension)
    assert(mods_on(0) && mods_on(1));
    assert(!mods_pending());

    // Off: to mods/disabled/, the folder made as needed, and what the game applies no longer sees it
    mods_set(0, false);
    assert(!mods_on(0) && exists("mods/disabled/link.bnp") && !exists("mods/link.bnp"));
    assert(mods_pending());
    mods_scan();
    assert(mods_count() == 2 && !mods_on(0) && mods_on(1));

    // On again, read as off at the last boot: back, and that is a change to apply
    mods_set(0, true);
    assert(mods_on(0) && exists("mods/link.bnp") && !exists("mods/disabled/link.bnp"));
    assert(mods_pending());
    mods_set(0, false);  // (and undoing it leaves nothing to apply)
    assert(!mods_pending());
    mods_set(0, true);

    // The same state twice changes nothing
    mods_set(0, true);
    assert(exists("mods/Zelda Hair.zip"));
    puts("PASS: mods_switch");
}
