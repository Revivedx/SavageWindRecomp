#!/bin/bash
# Build, run, and summarize: last log lines + symbolized guest backtrace.
cd "$(dirname "$0")/../.."
make -j16 2>&1 | grep -E 'error' -A3 && exit 1
SECS=${SECS:-30}
# Test runs play from a scratch copy of the save (default: the user's slot 0 only) so they are repeatable
# and never modify save/. REALSAVE=1 uses save/ directly.
if [ -z "$REALSAVE" ]; then
    rm -rf build/testsave && mkdir -p build/testsave/save_0
    cp save_backup_20261007/save_0/savedata0.bin save_backup_20261007/save_0/screenshot0.bin build/testsave/save_0/
    export SAVAGEWIND_SAVE_DIR=build/testsave
fi
. "$(dirname "$0")/game_slot.sh"; trap 'rm -rf "$GAME_SLOT"' EXIT  # (waits for 10 GB free)
${RUNPREFIX} ./build/savagewind extracted > build/run.log 2>&1 &
PID=$!
for i in $(seq 1 $SECS); do sleep 1; kill -0 $PID 2>/dev/null || break; done
kill -9 $PID 2>/dev/null && echo "(still running after ${SECS}s, killed)"
wait $PID 2>/dev/null
grep -v "^\[svc\]" build/run.log | grep -v "host backtrace" | grep -vE "^\s+[0-9]+ +(savagewind|libsystem)" | tail -${TAIL:-40}
ADDRS=$(grep -oE "f_[0-9a-f]{10}" build/run.log | head -${DEPTH:-14} | sed 's/f_//' | tr '\n' ' ')
[ -n "$ADDRS" ] && (cd tools && ./venv/bin/python sym.py $ADDRS)
