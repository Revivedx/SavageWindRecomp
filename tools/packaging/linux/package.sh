#!/bin/bash
# tools/packaging/linux/package.sh <x86_64|aarch64> (inside the amd64 build container, tools/packaging/linux/appimage.sh): the game built
# against build/linux/<arch>/deps and the game code generator helper (tools/packaging/linux/gen_helper.sh) put together
# as an AppDir and packed by appimagetool: build/dist/Savage_Wind-<version>-<arch>.AppImage.
# Shipped next to the executable (usr/bin, where runtime/paths.c rt_resource looks): what the start screen
# builds the game code with on the player's machine (runtime/gamebuild.c: swgen/, zig/ from tools/packaging/fetch_zig.sh,
# gamelib-src/), the window icon, the start screen font. No game code: that's built from the player's own files. usr/lib: the libraries from deps/ only - glibc, libstdc++, the Vulkan loader and the
# GPU drivers are the system's (a bundled loader would not find the user's drivers). No game files or fonts.
set -euo pipefail
cd "$(dirname "$0")/../../.."
ARCH=$1
L=$PWD/build/linux/$ARCH
P=$L/deps
B=$L/game
APPDIR=$L/AppDir
VERSION=$(sed -n 's/^#define SAVAGEWIND_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
OUT=build/dist/Savage_Wind-$VERSION-$ARCH.AppImage
JOBS=$(nproc)

tools/packaging/linux/build_deps.sh "$ARCH"

CROSSCC=clang
if [ "$ARCH" != "$(uname -m)" ]; then
    : "${SYSROOT:?cross build: SYSROOT (the arm64 root filesystem)}"
    CROSSCC="clang --target=$ARCH-linux-gnu --sysroot=$SYSROOT -fuse-ld=lld"
    MAKEARGS=(ARCH="$ARCH" SYSROOT="$SYSROOT")
else
    MAKEARGS=(ARCH="$ARCH")
fi

# the app alone (the game code is built on the player's machine)
make -j"$JOBS" B="$B" DEPS="$P" SHADERC=-lshaderc_shared "${MAKEARGS[@]}" "$B/savagewind"
if nm "$B/savagewind" | grep ' f_70' >/dev/null; then echo "$B/savagewind has game code linked in"; exit 1; fi
ZIG_TARGET=$ARCH-linux tools/packaging/fetch_zig.sh

# ---- AppDir
rm -rf "$APPDIR" && mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"
cp "$B/savagewind" "$APPDIR/usr/bin/"
llvm-strip --strip-debug "$APPDIR/usr/bin/savagewind"  # (symbols stay: crash backtraces name functions)
cp docs/icon/icon_1024.png "$APPDIR/usr/bin/icon.png"
cp third_party/fonts/FiraSans/FiraSans-SemiBold.ttf third_party/fonts/FiraSans/FiraSans-Medium.ttf "$APPDIR/usr/bin/"  # (menus, OFL)
cp third_party/fonts/FiraSans/OFL.txt "$APPDIR/usr/bin/FiraSans-OFL.txt"
cp README.md LICENSE "$APPDIR/usr/bin/"  # (licenses and source links of the bundled components: README "License")
[ -d "$L/helper/swgen" ] || { echo "no game code generator helper: tools/packaging/linux/gen_helper.sh $ARCH first"; exit 1; }
cp -R "$L/helper/swgen" build/zig "$APPDIR/usr/bin/"
mkdir "$APPDIR/usr/bin/gamelib-src"
cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/recomp/gamelib_entry.c runtime/hle/hle_symbols.txt \
   "$APPDIR/usr/bin/gamelib-src/"
cp tools/packaging/linux/AppRun "$APPDIR/" && chmod +x "$APPDIR/AppRun"
cp tools/packaging/linux/savagewind.desktop "$APPDIR/"
cp docs/icon/icon_1024.png "$APPDIR/savagewind.png" && ln -sf savagewind.png "$APPDIR/.DirIcon"

# libraries: what savagewind needs (DT_NEEDED, recursively) that deps/ provides; anything else must be a system one
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
SYSTEM_LIBS='^(libc|libm|libdl|libpthread|librt|libresolv|libstdc\+\+|libgcc_s|libvulkan)\.so|^ld-linux'
todo=("$APPDIR/usr/bin/savagewind")
while [ ${#todo[@]} -gt 0 ]; do
    f=${todo[0]}; todo=("${todo[@]:1}")
    for n in $(needed "$f"); do
        [ -e "$APPDIR/usr/lib/$n" ] && continue
        if [ -e "$P/lib/$n" ]; then
            cp -L "$P/lib/$n" "$APPDIR/usr/lib/$n"
            patchelf --set-rpath '$ORIGIN' "$APPDIR/usr/lib/$n"
            todo+=("$APPDIR/usr/lib/$n")
        elif ! [[ $n =~ $SYSTEM_LIBS ]]; then
            echo "package: $(basename "$f") needs $n, neither bundled nor a base system library"; exit 1
        fi
    done
done
patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/usr/bin/savagewind"
if ls "$APPDIR/usr/lib" | grep -iE 'x264|x265|xvid|postproc|vulkan'; then  # (no GPL codecs, no Vulkan loader)
    echo "package: library that must not ship"; exit 1
fi
echo "bundled: $(ls "$APPDIR/usr/lib" | tr '\n' ' ')"

# ---- AppImage (appimagetool runs extracted: no FUSE in the container)
fetch() {  # url sha256 file
    [ -f "build/linux/src/$3" ] || { curl -fsSL -o "build/linux/src/$3.part" "$1" && mv "build/linux/src/$3.part" "build/linux/src/$3"; }
    echo "$2  build/linux/src/$3" | sha256sum -c --quiet
}
fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
      ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 appimagetool-x86_64.AppImage
case $ARCH in
    x86_64) fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
                  2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d runtime-x86_64 ;;
    aarch64) fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-aarch64 \
                   00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444 runtime-aarch64 ;;
esac
chmod +x build/linux/src/appimagetool-x86_64.AppImage
mkdir -p build/dist && rm -f "$OUT"
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=$ARCH build/linux/src/appimagetool-x86_64.AppImage --no-appstream \
    --runtime-file "build/linux/src/runtime-$ARCH" "$APPDIR" "$OUT"
ls -la "$OUT"
