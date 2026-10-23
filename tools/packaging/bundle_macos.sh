#!/bin/bash
# make app: build/Savage Wind.app from <exe> (default build/mac15/savagewind). Contents/MacOS: the executable;
# Contents/Frameworks: every non-system dylib it loads (SDL3, FFmpeg; from build/deps, else Homebrew), load
# paths rewritten to @rpath; Contents/Resources: the icon and what the start screen builds the game code with on the player's
# machine (runtime/gamebuild.c): the generator helper (build/helper/swgen), the compiler (build/zig, tools/packaging/fetch_zig.sh)
# and the headers the code is compiled against (gamelib-src/). Ad-hoc signed. No game files, game code or fonts: the
# app keeps those in ~/Library/Application Support/<bundle id> (runtime/paths.h). One app: Option held while it opens
# shows the configuration screen (runtime/ui/launcher.mm launcher_config_key_held), Windows' "Savage Wind Configuration".
# MACOS_MIN=<x.y>: LSMinimumSystemVersion, and every Mach-O inside must be built for it or older (else it fails and
# lists them); without it the newest minimum found inside is used.
set -euo pipefail
cd "$(dirname "$0")/../.."
EXE=${1:-build/mac15/savagewind}
APP="build/Savage Wind.app"
DEPS=$PWD/build/deps
BUNDLE_ID=$(sed -n 's/^#define RT_BUNDLE_ID "\(.*\)"/\1/p' runtime/paths.h)
VERSION=$(sed -n 's/^#define SAVAGEWIND_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
# CFBundleShortVersionString must be digits: beta_<n> -> 0.9.<n>, v1.2.3 -> 1.2.3
case "$VERSION" in
    beta_*) VERSION="0.9.${VERSION#beta_}" ;;
    v*) VERSION="${VERSION#v}" ;;
    dev) VERSION="0.0.0" ;;
esac
GEN_HELPER=build/helper/swgen
ZIG=build/zig
[ -x "$EXE" ] || { echo "no $EXE"; exit 1; }
[ -x "$GEN_HELPER/swgen" ] || { echo "no $GEN_HELPER (make gen-helper)"; exit 1; }
[ -x "$ZIG/zig" ] || { echo "no $ZIG (tools/packaging/fetch_zig.sh)"; exit 1; }
# the app must hold no game code: that's built on the player's machine (grep without -q: with pipefail, nm cut off
# early would hide a match)
if nm "$EXE" | grep ' _f_70' >/dev/null; then echo "$EXE has game code linked in"; exit 1; fi

rm -rf "$APP"
C="$APP/Contents"
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources"
cp "$EXE" "$C/MacOS/savagewind"
strip -S "$C/MacOS/savagewind"  # (debug-map entries only: the symbols crash reports name functions by stay)
cp -R "$GEN_HELPER" "$C/Resources/swgen"
cp -R "$ZIG" "$C/Resources/zig"
mkdir "$C/Resources/gamelib-src"
cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/recomp/gamelib_entry.c runtime/hle/hle_symbols.txt \
   "$C/Resources/gamelib-src/"
cp docs/icon/icon.icns "$C/Resources/icon.icns"
cp docs/icon/icon_1024.png "$C/Resources/icon.png"  # (window / Dock icon at run time)
cp third_party/fonts/FiraSans/FiraSans-SemiBold.ttf third_party/fonts/FiraSans/FiraSans-Medium.ttf "$C/Resources/"  # (menus; OFL, README "License")
cp third_party/fonts/FiraSans/OFL.txt "$C/Resources/FiraSans-OFL.txt"

# ---- dylibs: copy every non-system dependency, recursively; point all references at @rpath/<name>
is_system() { case "$1" in /usr/lib/*|/System/*) return 0;; esac; return 1; }
find_dep() {  # dep as referenced by (the original of) $2 -> a real file
    case "$1" in
    @rpath/*|@loader_path/*)
        local n=${1#*/}
        for d in "$(dirname "$2")" "$DEPS/lib" /opt/homebrew/lib; do [ -e "$d/$n" ] && { echo "$d/$n"; return; }; done
        echo "cannot resolve $1 (from $2)" >&2; exit 1;;
    *) echo "$1";;
    esac
}
deps_of() { otool -L "$1" | tail -n +2 | awk '{print $1}' | grep -v "^$(otool -D "$1" | tail -n +2 | head -1)\$" || true; }
origin() { local s; s="$(dirname "$1")/.$(basename "$1").src"; if [ -f "$s" ]; then cat "$s"; else echo "$1"; fi; }
queue="$C/MacOS/savagewind"
copied=" "
while [ -n "$queue" ]; do
    f=${queue%%|*}; [ "$queue" = "$f" ] && queue= || queue=${queue#*|}
    for dep in $(deps_of "$f"); do
        is_system "$dep" && continue
        name=$(basename "$dep")
        if [[ "$copied" != *" $name "* ]]; then
            real=$(find_dep "$dep" "$(origin "$f")")
            cp -L "$real" "$C/Frameworks/$name"
            chmod u+w "$C/Frameworks/$name"
            install_name_tool -id "@rpath/$name" "$C/Frameworks/$name" 2>/dev/null
            echo "$real" > "$C/Frameworks/.$name.src"
            copied+="$name "
            queue=${queue:+$queue|}$C/Frameworks/$name
        fi
        [ "$dep" = "@rpath/$name" ] || install_name_tool -change "$dep" "@rpath/$name" "$f" 2>/dev/null
    done
done
rm -f "$C/Frameworks"/.*.src
rpaths() { otool -l "$1" | awk '/LC_RPATH/ {getline; getline; print $2}'; }
for r in $(rpaths "$C/MacOS/savagewind"); do install_name_tool -delete_rpath "$r" "$C/MacOS/savagewind"; done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$C/MacOS/savagewind"
for f in "$C/Frameworks"/*.dylib; do  # (their own build rpaths, if any: point them at the bundle instead)
    for r in $(rpaths "$f"); do install_name_tool -delete_rpath "$r" "$f"; done
    install_name_tool -add_rpath "@loader_path" "$f" 2>/dev/null || true
done

# ---- every Mach-O inside, its minimum macOS
machos() { find "$APP" -type f -perm +111 -o -type f -name '*.dylib' -o -type f -name '*.so' | while read -r f; do
    file -b "$f" | grep -q Mach-O && echo "$f"; done | sort -u; }
minos() { otool -l "$1" 2>/dev/null | awk '/LC_BUILD_VERSION/ {b=1} b && /minos/ {print $2; exit}
                                          /LC_VERSION_MIN_MACOSX/ {v=1} v && /version/ {print $2; exit}'; }
newer() { [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]; }  # $1 > $2
MIN=${MACOS_MIN:-}
TOO_NEW=""
NEWEST=11.0
while read -r f; do
    m=$(minos "$f"); [ -n "$m" ] || continue
    newer "$m" "$NEWEST" && NEWEST=$m
    [ -n "$MIN" ] && newer "$m" "$MIN" && TOO_NEW+="  ${f#$APP/} (macOS $m)"$'\n'
done < <(machos)
if [ -n "$TOO_NEW" ]; then printf 'built for newer than macOS %s:\n%s' "$MIN" "$TOO_NEW"; exit 1; fi
MIN=${MIN:-$NEWEST}

cat > "$C/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>savagewind</string>
    <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
    <key>CFBundleName</key><string>Savage Wind</string>
    <key>CFBundleDisplayName</key><string>Savage Wind</string>
    <key>CFBundleIconFile</key><string>icon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>LSMinimumSystemVersion</key><string>$MIN</string>
    <key>LSArchitecturePriority</key><array><string>arm64</string></array>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF
printf 'APPL????' > "$C/PkgInfo"

# ---- ad-hoc signature (Apple Silicon refuses to run unsigned code; the rewrites above invalidated the originals):
# every Mach-O inside first, then the bundle
while read -r f; do [ "$f" = "$C/MacOS/savagewind" ] || codesign -f -s - "$f" 2>/dev/null; done < <(machos)
codesign -f -s - "$APP"
codesign --verify --strict "$APP"
if find "$APP" -name 'swgame*' | grep . >/dev/null; then echo "game code inside $APP"; exit 1; fi
echo "$APP: $(du -sh "$APP" | cut -f1), $(ls "$C/Frameworks" | wc -l | tr -d ' ') dylibs, macOS $MIN+"
