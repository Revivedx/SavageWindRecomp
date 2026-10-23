#!/bin/bash
# Test screenshot run: tools/debug/shot.sh <frame> [max seconds] -- runs ./build/savagewind with the caller's SAVAGEWIND_* env,
# writes the window at <frame> and exits; killed after max seconds (default 150). Silent (SAVAGEWIND_NO_AUDIO=0: sound).
# Default: build/window.png + build/run_shot.log, one run at a time (lock). SHOT_ID=<name>: parallel run writing
# build/window_<name>.png + build/run_shot_<name>.log (give each parallel run its own SAVAGEWIND_SAVE_DIR too).
F=$1; T=${2:-150}
if [ -n "$SHOT_ID" ]; then
    PNG=build/window_$SHOT_ID.png; LOGF=build/run_shot_$SHOT_ID.log
else
    PNG=build/window.png; LOGF=build/run_shot.log
    until mkdir build/.shot.lock 2>/dev/null; do sleep 1; done
    LOCKED=1
fi
. "$(dirname "$0")/game_slot.sh"  # (waits for 10 GB free before starting a copy)
trap 'rm -rf "$GAME_SLOT"; [ -n "$LOCKED" ] && rmdir build/.shot.lock 2>/dev/null' EXIT
SAVAGEWIND_NO_AUDIO=${SAVAGEWIND_NO_AUDIO:-1} SAVAGEWIND_SHOT_PATH=$PNG SAVAGEWIND_WINDOW_SHOT=$F SAVAGEWIND_EXIT_AFTER_SHOT=1 ./build/savagewind extracted > "$LOGF" 2>&1 &
P=$!
for i in $(seq 1 $T); do kill -0 $P 2>/dev/null || exit 0; sleep 1; done
kill $P 2>/dev/null; echo "timed out" >&2; exit 1
