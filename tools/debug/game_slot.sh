# Sourced by the test scripts before they start ./build/savagewind: a new test copy of the game starts only while the
# Mac has at least 10 GB free (each copy takes ~2 GB; too many at once ran it out of memory). Launches are taken one
# at a time and the next waits ~20 s, so the copy just started has grown before memory is measured again.
# SAVAGEWIND_MIN_FREE_GB (default 10), SAVAGEWIND_SLOT_DIR.
GAME_MIN_FREE_GB=${SAVAGEWIND_MIN_FREE_GB:-10}
GAME_SLOT_DIR=${SAVAGEWIND_SLOT_DIR:-$HOME/.cache/savagewind_test_slots}
mkdir -p "$GAME_SLOT_DIR"
game_free_gb() {  # free + inactive + speculative + purgeable pages (what macOS can hand out without swapping)
    vm_stat | awk '/page size of/ {ps = $8} /Pages (free|inactive|speculative|purgeable):/ {gsub("\\.", "", $NF); n += $NF}
                   END {printf "%d", n * ps / 1073741824}'
}
L="$GAME_SLOT_DIR/launch"
until mkdir "$L" 2>/dev/null; do
    if ! kill -0 "$(cat "$L/pid" 2>/dev/null)" 2>/dev/null && [ -n "$(find "$L" -maxdepth 0 -mmin +2 2>/dev/null)" ]; then
        rm -rf "$L"  # (stale: holder gone for 2+ minutes)
    fi
    sleep 2
done
echo $$ > "$L/pid"
until [ "$(game_free_gb)" -ge "$GAME_MIN_FREE_GB" ]; do sleep 5; done
( sleep 20; rm -rf "$L" ) >/dev/null 2>&1 &
GAME_SLOT=  # (nothing to release at exit)
# test runs never write the player's savagewind.ini: a scratch copy unless the caller gave SAVAGEWIND_SETTINGS
if [ -z "$SAVAGEWIND_SETTINGS" ]; then
    mkdir -p build/test_ini
    SAVAGEWIND_SETTINGS="build/test_ini/settings_$$.ini"
    cp savagewind.ini "$SAVAGEWIND_SETTINGS" 2>/dev/null || : > "$SAVAGEWIND_SETTINGS"
    export SAVAGEWIND_SETTINGS
fi
