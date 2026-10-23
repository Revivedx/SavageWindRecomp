// Translate dumped shaders to MSL and compile them with Metal (runtime compiler).
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
#include "../../runtime/nvn/maxwell/shader.h"

int main(int argc, char **argv) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    int ok = 0, bad = 0, tfail = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        static uint8_t buf[0x10000];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        int stage = strstr(argv[i], "_s1_") ? STAGE_FRAGMENT : (strstr(argv[i], "cs_") || strstr(argv[i], "_s5_")) ? STAGE_COMPUTE : STAGE_VERTEX;
        // bound the code by the compiler-constant offset from the control dump, as the runtime does
        char cpath[512];
        snprintf(cpath, sizeof cpath, "%s", argv[i]);
        char *cp = strstr(cpath, "_code.bin");
        if (cp) {
            strcpy(cp, "_ctrl.bin");
            FILE *cf = fopen(cpath, "rb");
            if (cf) {
                uint8_t ctrl[0x800];
                if (fread(ctrl, 1, sizeof ctrl, cf) == sizeof ctrl) {
                    uint32_t coff = *(uint32_t *)(ctrl + 0x700);
                    if (coff && coff < n) n = coff;
                }
                fclose(cf);
            }
        }
        ShaderKey key = {0};
        key.flip_y = true;
        ShaderInfo *s = maxwell_translate(buf, n, stage, &key);
        if (!s->msl) { printf("%s: TRANSLATE FAIL: %s\n", argv[i], s->error); tfail++; continue; }
        char out[512];
        snprintf(out, sizeof out, "%s.metal", argv[i]);
        f = fopen(out, "w"); fputs(s->msl, f); fclose(f);
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(s->msl) options:nil error:&err];
        if (lib && [lib newFunctionWithName:@"main0"]) ok++;
        else { bad++; printf("%s: COMPILE FAIL\n%s\n", argv[i], err.localizedDescription.UTF8String); }
    }
    printf("ok %d, compile failures %d, translation failures %d\n", ok, bad, tfail);
    return 0;
}
