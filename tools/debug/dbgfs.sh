#!/bin/bash
# usage: tools/debug/dbgfs.sh <fs addr> <pc> <reg> <outname>  -- gameplay frame with one register visualized
cd "$(dirname "$0")/../.."
rm -f build/frame_*.png
IN="400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6"
SAVAGEWIND_EXIT_AFTER_SHOT=1 SAVAGEWIND_DBG_FS="$1:$2:$3" SAVAGEWIND_INPUT="$IN" SAVAGEWIND_SCREENSHOT_EVERY=2400 SECS=${SECS:-330} TAIL=1 tools/debug/run.sh >/dev/null 2>&1
cp build/frame_02400.png "build/dbg_$4.png" 2>/dev/null && echo "build/dbg_$4.png"
