// Translate dumped shaders to MSL and compile them with the Metal compiler.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/nvn/maxwell/shader.h"

int main(int argc, char **argv) {
    int ok = 0, bad = 0, tfail = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        static uint8_t buf[0x10000];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        int stage = strstr(argv[i], "_s1_") ? STAGE_FRAGMENT : STAGE_VERTEX;
        ShaderKey key = {0};
        key.flip_y = true;
        ShaderInfo *s = maxwell_translate(buf, n, stage, &key);
        if (!s->msl) { printf("%s: TRANSLATE FAIL: %s\n", argv[i], s->error); tfail++; continue; }
        char out[512];
        snprintf(out, sizeof out, "%s.metal", argv[i]);
        f = fopen(out, "w"); fputs(s->msl, f); fclose(f);
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "xcrun -sdk macosx metal -c %s -o /tmp/x.air 2> %s.err", out, out);
        if (system(cmd) == 0) ok++; else { bad++; printf("%s: METAL COMPILE FAIL (see %s.err)\n", argv[i], out); }
    }
    printf("translated+compiled %d, compile failures %d, translation failures %d\n", ok, bad, tfail);
    return 0;
}
