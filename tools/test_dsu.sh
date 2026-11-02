#!/bin/sh
# Linux, SDL3 headers/library and zlib; no game files or running DSU service needed.
# Optional: CC=clang DEPS=/path/to/deps SANITIZE=1 tools/test_dsu.sh
set -eu
cd "$(dirname "$0")/.."
B=${B:-build/tests}
mkdir -p "$B"
if [ -n "${DEPS:-}" ]; then
    includes="-I$DEPS/include"
    libs="-L$DEPS/lib -Wl,-rpath,$DEPS/lib -lSDL3"
else
    includes=$(pkg-config --cflags sdl3)
    libs=$(pkg-config --libs sdl3)
fi
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'; fi
${CC:-clang} -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-format \
    -ffunction-sections -fdata-sections $sanitize $includes -Iruntime \
    tools/test_dsu.c runtime/dsu.c -Wl,--gc-sections $libs -lpthread -lz -lm -o "$B/test_dsu"
"$B/test_dsu"
