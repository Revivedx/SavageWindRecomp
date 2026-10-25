#!/usr/bin/env bash
# Scripted test run from a saved point: the game loads a copy of the given save with "Continue", so a test is
# playable in ~30 s instead of starting a new game. tools/run.sh and tools/game_slot.sh are macOS only (vm_stat,
# save_backup_*). Never touches the player's saves or savagewind.ini: everything goes to build/test_run/.
#
#   tools/linux/test_run.sh <save folder> [seconds]     (from anywhere; default 90 s)
#
# <save folder>: one slot, with game_data.sav and caption.sav (as in the game's save folder: 0/, 1/...).
# Any SAVAGEWIND_* the caller sets is passed on, e.g. SAVAGEWIND_TEST_WARP=hateno SAVAGEWIND_TEST_TIME=12
# (runtime/services/test_warp.c). Game buttons after Continue: TEST_INPUT="1400:PLUS:6,1600:R:6" (the
# SAVAGEWIND_INPUT format, frames from start; playable from about frame 1100). SHOTS=<n>: a screenshot every n
# frames in build/test_run/shots/. SHOW=1 shows the window (hidden by default:
# SAVAGEWIND_HIDDEN hides it whatever its value).
set -euo pipefail
cd "$(dirname "$0")/../.."
save=${1:?usage: tools/linux/test_run.sh <save folder> [seconds]}
secs=${2:-90}
[ -f "$save/game_data.sav" ] || { echo "no game_data.sav in $save" >&2; exit 1; }
[ -x build/savagewind ] || { echo "build/savagewind not built" >&2; exit 1; }

d=build/test_run
rm -rf "$d" && mkdir -p "$d/save/save_0/0" "$d/shots"
cp "$save"/*.sav "$d/save/save_0/0/"
printf 'settings_version = 3\nmenu_tip_seen = 1\nprecompile_shaders = 0\n' > "$d/savagewind.ini"
# "Continue" is the first entry of the title menu: A until the save is loading (the title shows from about frame 250)
input=300:A:4,340:A:4,380:A:4,420:A:4,460:A:4,500:A:4,540:A:4${TEST_INPUT:+,$TEST_INPUT}

vars=(SAVAGEWIND_SETTINGS="$d/savagewind.ini" SAVAGEWIND_SAVE_DIR="$PWD/$d/save" SAVAGEWIND_LAUNCHER_AUTOPLAY=1
      SAVAGEWIND_INPUT="$input")
[ -n "${SHOW:-}" ] || vars+=(SAVAGEWIND_HIDDEN=1)
[ -n "${SHOTS:-}" ] && vars+=(SAVAGEWIND_SCREENSHOT_EVERY="$SHOTS")

rm -f build/frame_*.png
status=0
env "${vars[@]}" timeout -s KILL "$secs" ./build/savagewind extracted > "$d/run.log" 2>&1 || status=$?
mv build/frame_*.png "$d/shots/" 2>/dev/null || true
[ "$status" = 137 ] && echo "(stopped after ${secs}s)" || echo "(exited with $status)"
grep -E '^\[(warp|save)\]|mounted host' "$d/run.log" | sort | uniq -c | head -20 || true
echo "log: $d/run.log, $(find "$d/shots" -name '*.png' | wc -l) screenshots in $d/shots"
