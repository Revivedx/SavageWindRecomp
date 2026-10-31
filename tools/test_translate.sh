#!/bin/sh
# Fuzz test for runtime/nvn/maxwell/translate.c (see tools/test_translate.c): every GLSL translation compiles
# (shaderc, Vulkan 1.3) and validates (SPIRV-Tools). With REF set, also MSL output byte-identical to a reference
# MSL-only translator: REF is a commit holding one, e.g. LAS Recomp's 652e234 (the commit before its GLSL backend;
# not in this repository: git fetch --depth 1 https://github.com/peiikai/LASRecomp.git
# 652e2340f416d0472878ff1b756abc73b4fb9d8d). The Metal backend isn't maintained here (CONTRIBUTING.md).
# usage: [REF=<commit>] tools/test_translate.sh [count] [jobs] [mode glsl|glsl-bda|msl|both]
# Exits 1 when any job finds differences or compile / validation failures.
# Needs clang + shaderc + SPIRV-Tools (Windows: MSYS2 ucrt64; macOS: brew install shaderc spirv-tools; Debian /
# Ubuntu: libshaderc-dev spirv-tools; or GLSL=0 for the MSL comparison only).
set -e
cd "$(dirname "$0")/.."
COUNT=${1:-20000}
JOBS=${2:-8}
MODE=${3:-glsl}
OUT=build/test_translate
mkdir -p "$OUT/old" "$OUT/dump"
CC=${CC:-clang}
OBJS="$OUT/new.o"
DEFS=""
$CC -O2 -Wall -c runtime/nvn/maxwell/translate.c -o "$OUT/new.o"
if [ "$MODE" != glsl ] && [ "$MODE" != glsl-bda ]; then
  if [ -z "$REF" ]; then
    echo "mode $MODE compares the MSL with a reference translator: set REF (see the top of this script)" >&2
    exit 2
  fi
  git show "$REF:runtime/nvn/maxwell/translate.c" > "$OUT/old/translate.c"
  git show "$REF:runtime/nvn/maxwell/shader.h" > "$OUT/old/shader.h"
  # the reference's copies of every symbol the current translator defines get an old_ prefix
  RENAME=$(nm "$OUT/new.o" | awk '$2 ~ /^[TDBRC]$/ { s = $3; sub(/^_/, "", s); printf "-D%s=old_%s ", s, s }')
  PRE=""
  case "$(uname -s)" in MINGW*|MSYS*) PRE="-include malloc.h";; esac
  $CC -O2 -w $PRE $RENAME -c "$OUT/old/translate.c" -o "$OUT/old.o"
  OBJS="$OBJS $OUT/old.o"
  DEFS="-DMSL_REF"
fi
if [ "${GLSL:-1}" = 1 ]; then
  # shaderc is libshaderc_shared (MSYS2, Arch, from source) or libshaderc (Debian / Ubuntu, Homebrew); SPIRV-Tools is
  # shared where shipped, static C++ code behind its C API on Debian / Ubuntu (-lstdc++). Each probed with the call
  # the test makes, shared names first.
  SHADERC=-lshaderc_shared
  echo 'extern void *shaderc_compiler_initialize(void); int main(void){return shaderc_compiler_initialize()!=0;}' |
    $CC -x c - $SHADERC -o "$OUT/probe" 2>/dev/null || SHADERC=-lshaderc
  SPIRV=-lSPIRV-Tools-shared
  echo 'extern void *spvContextCreate(int); int main(void){return spvContextCreate(0)!=0;}' |
    $CC -x c - $SPIRV -o "$OUT/probe" 2>/dev/null || SPIRV="-lSPIRV-Tools -lstdc++"
  $CC -O2 -Wall $DEFS -DWITH_GLSL tools/test_translate.c $OBJS -o "$OUT/test_translate" $SHADERC $SPIRV
else
  $CC -O2 -Wall $DEFS tools/test_translate.c $OBJS -o "$OUT/test_translate"
fi
rm -f "$OUT"/dump/* "$OUT"/log_*.txt
PER=$(( (COUNT + JOBS - 1) / JOBS ))
PIDS=""
i=0
while [ $i -lt "$JOBS" ]; do
  "$OUT/test_translate" runtime/nvn/maxwell/translate.c "$PER" $((i * 7919 + ${SEED:-1})) "$MODE" "$OUT/dump" > "$OUT/log_$i.txt" 2>&1 &
  PIDS="$PIDS $!"
  i=$((i + 1))
done
FAIL=0
for p in $PIDS; do wait "$p" || FAIL=1; done
cat "$OUT"/log_*.txt
exit $FAIL
