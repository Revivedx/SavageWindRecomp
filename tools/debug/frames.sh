#!/bin/bash
# Frame-series capture: tools/debug/frames.sh <from frame> <count> [every]  -- runs ./build/savagewind with the caller's
# SAVAGEWIND_* env and saves every Nth frame from <from> into build/frames/ (cleared first), then stops.
FROM=$1; N=$2; E=${3:-3}
rm -rf build/frames
. "$(dirname "$0")/game_slot.sh"; trap 'rm -rf "$GAME_SLOT"' EXIT  # (waits for 10 GB free)
SAVAGEWIND_NO_AUDIO=${SAVAGEWIND_NO_AUDIO:-1} SAVAGEWIND_SCREENSHOT_EVERY=$E SAVAGEWIND_SCREENSHOT_FROM=$FROM ./build/savagewind extracted > build/run_shot.log 2>&1 &
P=$!
for i in $(seq 1 240); do sleep 1; n=$(ls build/frames 2>/dev/null | wc -l); [ "$n" -ge "$N" ] && break; kill -0 $P 2>/dev/null || break; done
kill $P 2>/dev/null; ls build/frames | wc -l
