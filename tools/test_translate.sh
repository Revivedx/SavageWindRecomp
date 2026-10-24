#!/bin/sh
# Fuzz test for runtime/nvn/maxwell/translate.c (see tools/test_translate.c):
#   MSL output byte-identical to the reference MSL-only translator (REF, default: the commit before the GLSL
#   backend), and every GLSL translation compiles (shaderc, Vulkan 1.3) and validates (SPIRV-Tools).
# usage: tools/test_translate.sh [count] [jobs] [mode msl|glsl|both]
# Needs clang + shaderc + SPIRV-Tools (Windows: MSYS2 ucrt64; macOS: brew install shaderc spirv-tools, or
# GLSL=0 for the MSL comparison only).
# The reference commit is from LAS Recomp's history, which this repository doesn't have: the msl and both modes need
#   git fetch --depth 1 https://github.com/peiikai/LASRecomp.git 652e2340f416d0472878ff1b756abc73b4fb9d8d
# Exits non-zero when any job finds a difference or a GLSL compile or validation failure.
set -e
cd "$(dirname "$0")/.."
COUNT=${1:-20000}
JOBS=${2:-8}
MODE=${3:-both}
REF=${REF:-652e234}
OUT=build/test_translate
mkdir -p "$OUT/old" "$OUT/dump"
CC=${CC:-clang}
RENAME="-Dmaxwell_translate=old_maxwell_translate -Dmaxwell_free=old_maxwell_free
 -Dmaxwell_disassemble=old_maxwell_disassemble -Dmaxwell_gmem_base=old_maxwell_gmem_base
 -Dmaxwell_gmem_size=old_maxwell_gmem_size -Dmaxwell_dbg_code=old_maxwell_dbg_code -Dmaxwell_dbg_pc=old_maxwell_dbg_pc
 -Dmaxwell_dbg_reg=old_maxwell_dbg_reg -Dmaxwell_dbg_vs=old_maxwell_dbg_vs"
PRE=""
case "$(uname -s)" in MINGW*|MSYS*) PRE="-include malloc.h";; esac
if [ "$MODE" = glsl ]; then
  # test_translate links the reference translator but only the MSL comparison calls it: a stub stands in for it
  cat > "$OUT/old/stub.c" <<'STUB'
#include "../../../runtime/nvn/maxwell/shader.h"
ShaderInfo *old_maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key) {
    (void)code; (void)max_size; (void)stage; (void)key;
    return NULL;
}
void old_maxwell_free(ShaderInfo *s) { (void)s; }
uint64_t old_maxwell_gmem_base, old_maxwell_gmem_size;
STUB
  $CC -O2 -Wall $PRE -c "$OUT/old/stub.c" -o "$OUT/old.o"
elif git cat-file -e "$REF^{commit}" 2>/dev/null; then
  git show "$REF:runtime/nvn/maxwell/translate.c" > "$OUT/old/translate.c"
  git show "$REF:runtime/nvn/maxwell/shader.h" > "$OUT/old/shader.h"
  $CC -O2 -w $PRE $RENAME -c "$OUT/old/translate.c" -o "$OUT/old.o"
else
  echo "test_translate.sh: reference commit $REF not found; fetch it (see the header) or use the glsl mode" >&2
  exit 1
fi
$CC -O2 -Wall -c runtime/nvn/maxwell/translate.c -o "$OUT/new.o"
if [ "${GLSL:-1}" = 1 ]; then
  $CC -O2 -Wall -DWITH_GLSL tools/test_translate.c "$OUT/new.o" "$OUT/old.o" -o "$OUT/test_translate" \
    -lshaderc_shared -lSPIRV-Tools-shared
else
  $CC -O2 -Wall tools/test_translate.c "$OUT/new.o" "$OUT/old.o" -o "$OUT/test_translate"
fi
rm -f "$OUT"/dump/* "$OUT"/log_*.txt
PER=$(( (COUNT + JOBS - 1) / JOBS ))
i=0
pids=""
while [ $i -lt "$JOBS" ]; do
  "$OUT/test_translate" runtime/nvn/maxwell/translate.c "$PER" $((i * 7919 + ${SEED:-1})) "$MODE" "$OUT/dump" > "$OUT/log_$i.txt" 2>&1 &
  pids="$pids $!"
  i=$((i + 1))
done
status=0
for pid in $pids; do
  wait "$pid" || status=1
done
cat "$OUT"/log_*.txt
exit $status
