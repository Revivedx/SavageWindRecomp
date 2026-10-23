#!/bin/bash
# Differential test of runtime/nvn/astc.c against ARM's astcenc (Apache-2.0), built from source into
# build/ (gitignored, never committed). Usage: tools/tests/test_astc.sh [random blocks per block size] [seed]
# Windows: needs the MSYS2 ucrt64 clang toolchain. macOS: Xcode clang works too.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
N="${1:-1048576}"
SEED="${2:-1}"
[ -d /c/msys64/ucrt64/bin ] && export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
EXE=""; [ -n "$WINDIR" ] && EXE=".exe"

REF="$ROOT/build/astc-encoder"
SRC="$REF/Source"
OUT="$ROOT/build/astc_test"
mkdir -p "$OUT/obj"
[ -d "$REF" ] || git clone --depth 1 https://github.com/ARM-software/astc-encoder.git "$REF"
echo "astcenc: $(git -C "$REF" log -1 --format='%h %s')"

# astcenc: portable (no-SIMD) build
DEFS="-DASTCENC_NEON=0 -DASTCENC_SVE=0 -DASTCENC_SSE=0 -DASTCENC_AVX=0 -DASTCENC_POPCNT=0 -DASTCENC_F16C=0"
sed -e 's/@astcencoder_VERSION@/local/' -e 's/@astcencoder_YEAR@/2026/' "$SRC/astcenccli_version.h.in" > "$OUT/obj/astcenccli_version.h"
# MinGW: winpthreads' pthread.h clashes with the CLI's Win32 pthread emulation -> rename it
sed -e 's/^#include <cstring>$/#include <cstring>\n#define pthread_t ae_pthread_t\n#define pthread_attr_t ae_pthread_attr_t\n#define pthread_create ae_pthread_create\n#define pthread_join ae_pthread_join/' \
    "$SRC/astcenccli_platform_dependents.cpp" > "$OUT/obj/astcenccli_platform_dependents.cpp"
pids=()
for f in "$SRC"/*.cpp "$OUT/obj/astcenccli_platform_dependents.cpp"; do
    [ "$f" = "$SRC/astcenccli_platform_dependents.cpp" ] && continue
    o="$OUT/obj/$(basename "$f" .cpp).o"
    [ -f "$o" ] && continue
    clang++ -std=c++17 -O2 -DNDEBUG $DEFS -I"$OUT/obj" -I"$SRC" -c "$f" -o "$o" &
    pids+=($!)
done
for p in "${pids[@]}"; do wait "$p"; done
LIB=$(ls "$OUT"/obj/astcenc_*.o)
[ -f "$OUT/astcenc$EXE" ] || clang++ "$OUT"/obj/*.o -o "$OUT/astcenc$EXE"

clang++ -std=c++17 -O2 $DEFS -I"$SRC" -c "$ROOT/tools/tests/test_astc_ref.cpp" -o "$OUT/test_astc_ref.o"
clang -std=c11 -O2 -Wall -Wextra -c "$ROOT/runtime/nvn/astc.c" -o "$OUT/astc.o"
clang -std=c11 -O2 -Wall -c "$ROOT/tools/tests/test_astc.c" -o "$OUT/test_astc.o"
clang++ "$OUT/test_astc.o" "$OUT/astc.o" "$OUT/test_astc_ref.o" $LIB -o "$OUT/test_astc$EXE"
T="$OUT/test_astc$EXE"
A="$OUT/astcenc$EXE"
[ "$N" = build ] && exit 0  # tools/tests/test_astc.sh build: just build (then run build/astc_test/test_astc blk ...)

fail=0
echo "== 1. astcenc CLI round trip (-cl/-cs then -dl/-ds into .tga) =="
mkdir -p "$OUT/cli"
"$T" mkimg "$OUT/cli/in.tga" 157 93 "$SEED"
for bs in 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12; do
    for q in -fastest -medium -thorough; do
        for p in l s; do
            "$A" -c$p "$OUT/cli/in.tga" "$OUT/cli/t.astc" $bs $q -silent >/dev/null
            "$A" -d$p "$OUT/cli/t.astc" "$OUT/cli/t.tga" -silent >/dev/null
            srgb=0; [ $p = s ] && srgb=1
            "$T" cmp "$OUT/cli/t.astc" "$OUT/cli/t.tga" $srgb | grep -v ': 0 mismatching' || true
            "$T" cmp "$OUT/cli/t.astc" "$OUT/cli/t.tga" $srgb >/dev/null || fail=1
        done
    done
done
echo "CLI round trip done"
echo "== 2. astcenc CLI on random-block .astc files (-dl/-ds) =="
for bs in "4 4" "6 6" "8 5" "8 8" "10 10" "12 12"; do
    set -- $bs
    "$T" mkrand "$OUT/cli/r.astc" $1 $2 $((64 * $1)) $((64 * $2)) "$SEED"
    for p in l s; do
        srgb=0; [ $p = s ] && srgb=1
        "$A" -d$p "$OUT/cli/r.astc" "$OUT/cli/r.tga" -silent >/dev/null
        "$T" cmp "$OUT/cli/r.astc" "$OUT/cli/r.tga" $srgb || fail=1
    done
done
echo "== 3. library: synthetic images, every block size, 4 qualities, LDR + sRGB =="
"$T" img "$SEED" || fail=1
echo "== 4. library: $N random blocks per block size =="
"$T" rand "$N" "$SEED" || fail=1
echo "== 5. throughput =="
"$T" bench
[ $fail = 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
