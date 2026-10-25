#!/bin/sh
# Build the app (start screen only, no game code) natively in each distribution's container. What differs between
# distributions is exactly what a container reproduces: package names, whether SDL3 / FFmpeg / shaderc exist as
# packages and what their libraries are called (libshaderc.so vs libshaderc_shared.so), lld, and the clang version
# (the lifted FP code needs __builtin_elementwise_*). A virtual machine per distribution would test the same thing
# an order of magnitude slower; QEMU matters for other CPU architectures (the aarch64 AppImage), not for this.
#
# Usage: tools/linux/distro_build.sh [image...]     (default: every distribution in DISTROS)
#        ENGINE=docker|podman JOBS=8 tools/linux/distro_build.sh archlinux:latest
# The source tree is mounted read-only and objects go to /tmp/build inside the container: a run leaves nothing behind.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ENGINE=${ENGINE:-$(command -v podman || command -v docker)}
JOBS=${JOBS:-$(nproc)}
PLATFORM=--platform=linux/amd64

# Every distribution here has all its dependencies in its own archives, as the app needs them (see docs/linux.md).
# Ubuntu 24.04 LTS and older, Debian 12 and openSUSE Leap are not in the list: no SDL3 package (build SDL3 from
# source, or use the AppImage).
DISTROS="debian:13 ubuntu:26.04 fedora:44 archlinux:latest opensuse/tumbleweed:latest"

deps_for() {
    case $1 in
        debian*|ubuntu*)  # SDL3 3.2+, clang 19+ / 20+, libshaderc.so
            echo 'apt-get update -qq; apt-get install -y -qq --no-install-recommends make clang lld pkg-config \
                libsdl3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
                libvulkan-dev libshaderc-dev zlib1g-dev' ;;
        fedora*)          # SDL3-devel, libshaderc-devel, FFmpeg as the "free" variant
            echo 'dnf install -y -q make clang lld pkgconf-pkg-config SDL3-devel libshaderc-devel vulkan-loader-devel \
                ffmpeg-free-devel libavformat-free-devel libavcodec-free-devel libavutil-free-devel libswscale-free-devel zlib-devel' ;;
        arch*)
            echo 'pacman -Syu --noconfirm --quiet --needed \
                base-devel clang lld sdl3 shaderc vulkan-icd-loader vulkan-headers ffmpeg zlib' ;;
        opensuse*)        # Tumbleweed: libavformat-devel is its FFmpeg (headers in /usr/include/ffmpeg), SDL3 as SDL3-devel
            echo 'zypper -q --non-interactive --gpg-auto-import-keys refresh; zypper -q --non-interactive \
                install --no-recommends make clang lld pkgconf-pkg-config SDL3-devel shaderc-devel vulkan-devel \
                libavformat-devel libavcodec-devel libavutil-devel libswscale-devel zlib-devel' ;;
        *) return 2 ;;
    esac
}

images=${*:-$DISTROS}
failed=
for image in $images; do
    if ! install=$(deps_for "$image"); then
        echo "distro_build.sh: no dependency list for $image" >&2
        failed="$failed $image"
        continue
    fi
    echo "=== $image"
    log=$(mktemp)
    if "$ENGINE" run --rm $PLATFORM -v "$REPO":/src:ro -w /src \
            -e SW_INSTALL="$install" -e JOBS="$JOBS" "$image" \
            sh -c 'eval "$SW_INSTALL" || exit 1; make -j"$JOBS" B=/tmp/build || exit 1; \
                   ldd /tmp/build/savagewind | grep -E "SDL3|avformat|shaderc"' \
            > "$log" 2>&1 < /dev/null; then
        echo "=== $image OK"
        grep -E "SDL3|avformat|shaderc" "$log" | sed "s/^/    /"
    else
        echo "=== $image FAILED (full log: $log)"
        tail -40 "$log" | sed "s/^/    /"
        failed="$failed $image"
        continue
    fi
    rm -f "$log"
done
if [ -n "$failed" ]; then echo "failed:$failed" >&2; exit 1; fi
echo "all distributions built"
