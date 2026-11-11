#!/bin/sh
# Builds build/ach_tool, then builds and runs the achievements tests (no SDL / game files needed).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CORE="runtime/achievements.c runtime/achievements_defs.c runtime/achievements_synth.c runtime/achievements_bars.c"
cc -Wall -Wextra -O1 tools/ach_tool.c $CORE -o build/ach_tool -lpthread -lm
echo "built build/ach_tool"
cc -Wall -Wextra -O1 -DACH_STANDALONE -Iruntime tools/test_achievements.c $CORE runtime/achievements_watch.c -o build/test_achievements -lpthread -lm
build/test_achievements
