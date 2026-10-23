#!/bin/bash
# make appimage ARCH=<x86_64|aarch64>: build/dist/Savage_Wind-<version>-<arch>.AppImage, built in Ubuntu 22.04
# containers (podman; tools/packaging/linux/Containerfile) so it runs on glibc 2.35+ distributions. Everything is built inside
# the amd64 container - aarch64 cross-compiled against the arm64 image's root filesystem (build/linux/aarch64/sysroot)
# - except the generator helpers, which PyInstaller freezes in a container of their own arch (arm64: QEMU, binfmt).
# No game code inside: the start screen builds it on the player's machine. See docs/linux.md.
set -euo pipefail
cd "$(dirname "$0")/../../.."
ARCH=${1:?usage: tools/packaging/linux/appimage.sh x86_64|aarch64}
case $ARCH in x86_64) PODARCH=amd64 ;; aarch64) PODARCH=arm64 ;;
    *) echo "unknown arch $ARCH"; exit 1 ;; esac

image() {  # arch -> tag (built once, then cached by podman)
    podman build -q --arch "$1" -t "localhost/savagewind-build:$1" -f tools/packaging/linux/Containerfile tools/packaging/linux >/dev/null
    echo "localhost/savagewind-build:$1"
}
run() {  # image, command...: the checkout mounted at /src
    local img=$1; shift
    podman run --rm --arch "${img##*:}" -v "$PWD:/src" -w /src -e SYSROOT=/src/build/linux/aarch64/sysroot "$img" "$@"
}

AMD=$(image amd64)
if [ "$ARCH" = aarch64 ]; then
    ARM=$(image arm64)
    S=build/linux/aarch64/sysroot
    if [ ! -f "$S/.ok" ]; then  # the arm64 image's files: headers and libraries to cross-compile against
        rm -rf "$S" && mkdir -p "$S"
        c=$(podman create --arch arm64 "$ARM")
        # (anchored: only the top-level pseudo filesystems, not e.g. usr/include/<triple>/sys)
        podman export "$c" | tar -x -C "$S" --anchored --exclude='dev/*' --exclude='proc/*' --exclude='sys/*' 2>/dev/null || true
        podman rm -f "$c" >/dev/null
        # absolute symlinks (libm.so -> /lib/aarch64-linux-gnu/libm.so.6) would point into this machine's root:
        # relative, so they stay inside the sysroot
        find "$S" -type l -lname '/*' | while read -r l; do
            ln -sfn "$(realpath -m --relative-to="$(dirname "$l")" "$S$(readlink "$l")")" "$l"
        done
        touch "$S/.ok"
    fi
    run "$ARM" tools/packaging/linux/gen_helper.sh aarch64
else
    run "$AMD" tools/packaging/linux/gen_helper.sh x86_64
fi
run "$AMD" tools/packaging/linux/package.sh "$ARCH"
