#!/bin/bash
# tools/packaging/linux/build_deps.sh <x86_64|aarch64> (inside the build container, tools/packaging/linux/appimage.sh): what the AppImage
# links against and ships, from pinned sources into build/linux/<arch>/deps - SDL3, an LGPL-only FFmpeg with just what
# the game's movies need (the configuration of tools/packaging/build_deps_windows.sh), shaderc (GLSL -> SPIR-V; Ubuntu 22.04 has
# none), and the headers: Vulkan (newer than 22.04's), Vulkan Memory Allocator. aarch64 is cross-compiled against
# the arm64 image's root filesystem in $SYSROOT. Each part is skipped when its output exists (delete deps/ to redo).
set -euo pipefail
cd "$(dirname "$0")/../../.."
ARCH=$1
S=$PWD/build/linux/src
P=$PWD/build/linux/$ARCH/deps
W=$PWD/build/linux/$ARCH/deps-build
mkdir -p "$S" "$P/include" "$W"
JOBS=$(nproc)

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || { curl -fsSL -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | sha256sum -c --quiet
}
unpack() { rm -rf "$W/$2" && mkdir -p "$W/$2" && tar -xf "$S/$1" -C "$W/$2" --strip-components 1; }

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
      7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68 SDL3-3.4.16.tar.gz
fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz
fetch https://github.com/google/shaderc/archive/refs/tags/v2026.3.tar.gz \
      ee493ccf1b3038b4ef2fe024664c5eb2dc4bcc1f6b05b33e3909de0e19c81024 shaderc-2026.3.tar.gz
fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
      6d1bb65e49520344cc0a48af3dc02e993781efff14c7ebdcb8ae9fa23ddf7e83 Vulkan-Headers-1.4.350.1.tar.gz
fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz \
      c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e VulkanMemoryAllocator-3.3.0.tar.gz

# cross build: clang targets aarch64 against the sysroot (the arm64 image's -dev packages)
CMAKE_CROSS=()
FF_CROSS=()
if [ "$ARCH" != "$(uname -m)" ]; then
    : "${SYSROOT:?cross build: SYSROOT (the arm64 root filesystem)}"
    TC=$W/toolchain.cmake
    cat > "$TC" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $ARCH)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET $ARCH-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET $ARCH-linux-gnu)
set(CMAKE_EXE_LINKER_FLAGS_INIT -fuse-ld=lld)
set(CMAKE_SHARED_LINKER_FLAGS_INIT -fuse-ld=lld)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
    CMAKE_CROSS=(-DCMAKE_TOOLCHAIN_FILE="$TC")
    export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
    export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/$ARCH-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig
    FF_CROSS=(--enable-cross-compile --sysroot="$SYSROOT" --cc="clang --target=$ARCH-linux-gnu --sysroot=$SYSROOT"
              --cxx="clang++ --target=$ARCH-linux-gnu --sysroot=$SYSROOT" --ld="clang --target=$ARCH-linux-gnu --sysroot=$SYSROOT -fuse-ld=lld"
              --nm=llvm-nm --ar=llvm-ar --ranlib=llvm-ranlib --strip=llvm-strip)
else
    FF_CROSS=(--cc=clang --cxx=clang++)
fi

if [ ! -f "$P/include/vulkan/vulkan.h" ]; then
    unpack Vulkan-Headers-1.4.350.1.tar.gz vulkan-headers
    cp -R "$W/vulkan-headers/include/vulkan" "$W/vulkan-headers/include/vk_video" "$P/include/"
fi
if [ ! -f "$P/include/vma/vk_mem_alloc.h" ]; then
    unpack VulkanMemoryAllocator-3.3.0.tar.gz vma
    mkdir -p "$P/include/vma" && cp "$W/vma/include/vk_mem_alloc.h" "$P/include/vma/"
fi

if [ ! -f "$P/lib/libSDL3.so" ]; then
    unpack SDL3-3.4.16.tar.gz sdl
    cmake -S "$W/sdl" -B "$W/sdl/build" -G Ninja "${CMAKE_CROSS[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF -DSDL_RPATH=OFF
    cmake --build "$W/sdl/build" -j"$JOBS" && cmake --install "$W/sdl/build"
fi

if [ ! -f "$P/lib/libavcodec.so" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$W/ffmpeg" && ./configure --prefix="$P" "${FF_CROSS[@]}" --arch="$ARCH" --target-os=linux \
        --enable-shared --disable-static --enable-pic \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        && make -j"$JOBS" && make install)
fi
grep -q 'GPL version 3\|--enable-gpl' "$P/lib/pkgconfig/libavcodec.pc" && { echo "deps: FFmpeg is not LGPL"; exit 1; }

# shaderc as one shared library (glslang, SPIRV-Tools inside), the libshaderc_shared the Windows build links too;
# its DEPS file pins the glslang / SPIRV-Tools / SPIRV-Headers commits (utils/git-sync-deps clones them)
if [ ! -f "$P/lib/libshaderc_shared.so" ]; then
    unpack shaderc-2026.3.tar.gz shaderc
    (cd "$W/shaderc" && python3 utils/git-sync-deps)
    cmake -S "$W/shaderc" -B "$W/shaderc/build" -G Ninja "${CMAKE_CROSS[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_LIBDIR=lib -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF \
        -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF
    cmake --build "$W/shaderc/build" --target shaderc_shared -j"$JOBS"
    mkdir -p "$P/include/shaderc"
    cp "$W/shaderc/libshaderc/include/shaderc/"*.h "$P/include/shaderc/"
    cp -P "$W/shaderc/build/libshaderc/libshaderc_shared.so"* "$P/lib/"
fi
echo "deps ($ARCH): $(cd "$P/lib" && ls *.so | tr '\n' ' ')"
