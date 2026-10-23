#!/bin/bash
# The compiler the start screen builds the game code with on the player's machine (runtime/gamebuild.c): zig (MIT,
# its clang / lld: Apache-2.0 with LLVM exception), as `zig cc`. Fetches the pinned release for this OS / CPU (or
# ZIG_TARGET=<arch>-<os>, e.g. x86_64-windows) into build/zig/, checks its hash and keeps only what compiling C for
# that same machine needs. Shipped as Contents/Resources/zig (macOS), zig/ next to the exe (Windows, Linux).
set -euo pipefail
cd "$(dirname "$0")/../.."
VER=0.17.0
case "$(uname -s)" in Darwin) OS=macos;; Linux) OS=linux;; *) OS=windows;; esac
case "$(uname -m)" in arm64|aarch64) ARCH=aarch64;; *) ARCH=x86_64;; esac
TARGET=${ZIG_TARGET:-$ARCH-$OS}
EXT=tar.xz; [ "${TARGET#*-}" = windows ] && EXT=zip
NAME=zig-$TARGET-$VER
# sha256 of each release archive (ziglang.org/download/index.json; checked against it when pinned)
case "$TARGET" in
    aarch64-macos)  SHA=b607e9b9234790a008116ae5bdb71c6243b84b9fb42a53a9e70fde41c06c536a ;;
    x86_64-linux)   SHA=1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026 ;;
    aarch64-linux)  SHA=9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8 ;;
    x86_64-windows) SHA=b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428 ;;
    *) echo "no zig for $TARGET"; exit 1;;
esac
[ -n "$SHA" ] || { echo "tools/packaging/fetch_zig.sh: pin the sha256 of $NAME.$EXT first"; exit 1; }

DL=build/zig-dl
mkdir -p "$DL"
[ -f "$DL/$NAME.$EXT" ] || curl -fL --proto '=https' -o "$DL/$NAME.$EXT" "https://ziglang.org/download/$VER/$NAME.$EXT"
GOT=$( (shasum -a 256 2>/dev/null || sha256sum) < "$DL/$NAME.$EXT" | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || { echo "$NAME.$EXT: sha256 $GOT, expected $SHA"; exit 1; }
rm -rf "$DL/$NAME" build/zig
if [ "$EXT" = zip ]; then unzip -q "$DL/$NAME.$EXT" -d "$DL"; else tar -xJf "$DL/$NAME.$EXT" -C "$DL"; fi
mv "$DL/$NAME" build/zig

# size: only what compiling C for this same OS needs (the game code is built for the machine it runs on): its libc
# headers and libc sources; no C++ runtimes (libcxx, libcxxabi) or sanitizers
L=build/zig/lib
case "${TARGET#*-}" in
    macos)   inc='darwin'; src='darwin' ;;
    linux)   inc='linux|glibc'; src='glibc' ;;
    windows) inc='windows|mingw'; src='mingw' ;;
esac
for d in "$L"/libc/include/*; do basename "$d" | grep -qE "$inc" || rm -rf "$d"; done
for d in "$L"/libc/*; do n=$(basename "$d"); [ "$n" = include ] || echo "$n" | grep -qE "^($src)$" || rm -rf "$d"; done
rm -rf "$L/libcxx" "$L/libcxxabi" "$L/libtsan" "$L/docs" build/zig/doc  # (lib/std stays: zig cc builds compiler_rt with it)
echo "build/zig: zig $VER for $TARGET, $(du -sh build/zig | cut -f1)"
