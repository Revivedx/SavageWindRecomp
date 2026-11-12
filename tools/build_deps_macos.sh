#!/bin/bash
# make deps (macOS): the libraries and tools the shipped .app contains, built from pinned sources for the app's
# minimum macOS (MACOS_MIN, default 15.0) into build/deps. Homebrew's bottles target the macOS they were built on, so
# they can't ship. Plain `make` (dev builds) keeps using Homebrew.
#   SDL3; FFmpeg (LGPL only, no GPL parts: just what the game's movies need - WebM: VP9 video + Vorbis audio, plus
#   VP8 / Opus / H.264 / AAC in WebM or MP4 - and swscale);
#   a relocatable CPython 3.12 (python-build-standalone, built for macOS 11+) that `make gen-helper` freezes.
# Needs: Xcode command line tools, cmake. Each step is skipped when its output exists (delete build/deps/<x> to redo).
set -euo pipefail
cd "$(dirname "$0")/.."
MIN=${MACOS_MIN:-15.0}
P=$PWD/build/deps
S=$P/src
mkdir -p "$S" "$P/bin" "$P/lib"
export MACOSX_DEPLOYMENT_TARGET=$MIN
JOBS=$(sysctl -n hw.ncpu)

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | shasum -a 256 -c --quiet
}
unpack() { rm -rf "$S/$2" && mkdir -p "$S/$2" && tar -xf "$S/$1" -C "$S/$2" --strip-components 1; }

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
      9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 SDL3-3.4.18.tar.gz
fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz
fetch "https://github.com/astral-sh/python-build-standalone/releases/download/20260924/cpython-3.12.14%2B20260924-aarch64-apple-darwin-install_only.tar.gz" \
      9763f43db2481a6af36af82ec40302aab7a73632f880129d07a6e81aec846277 cpython-3.12.14-20260924.tar.gz

if [ ! -f "$P/lib/libSDL3.0.dylib" ]; then
    unpack SDL3-3.4.18.tar.gz sdl
    cmake -S "$S/sdl" -B "$S/sdl/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DCMAKE_OSX_ARCHITECTURES=arm64 -DSDL_SHARED=ON -DSDL_STATIC=OFF \
          -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
    cmake --build "$S/sdl/build" -j"$JOBS" && cmake --install "$S/sdl/build"
fi

if [ ! -f "$P/lib/libavcodec.dylib" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$S/ffmpeg" && ./configure --prefix="$P" --cc=clang --arch=arm64 --enable-shared --disable-static \
        --extra-cflags="-mmacosx-version-min=$MIN" --extra-ldflags="-mmacosx-version-min=$MIN" \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        --install-name-dir='@rpath' && make -j"$JOBS" && make install)
fi

# The Vulkan build (RENDERER, default vulkan; MoltenVK): the Vulkan loader and shaderc (with the glslang / SPIRV-Tools / SPIRV-Headers it builds
# from), pinned to one Vulkan SDK release. MoltenVK itself is copied in by tools/bundle_macos.sh.
if [ "${RENDERER:-vulkan}" = vulkan ]; then
    VK=1.4.363.0
    fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-$VK.tar.gz \
          4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e Vulkan-Headers-$VK.tar.gz
    fetch https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-$VK.tar.gz \
          941eff558fc74f248745fb7df367640cf89a8457f216dae498a74ae14b6ba6f7 Vulkan-Loader-$VK.tar.gz
    fetch https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-$VK.tar.gz \
          5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9 SPIRV-Headers-$VK.tar.gz
    fetch https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/vulkan-sdk-$VK.tar.gz \
          e6c83a215538fbfd265ccf398d3877985055df1273dbed73912bd05307664688 SPIRV-Tools-$VK.tar.gz
    fetch https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz \
          9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2 glslang-16.6.0.tar.gz
    fetch https://github.com/google/shaderc/archive/refs/tags/v2026.4.tar.gz \
          f06ce5bcca94e5df7f34e115743597d0ad2e13c5fe9213c67dc8c76031241947 shaderc-2026.4.tar.gz
    CM=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_PREFIX_PATH="$P"
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_INSTALL_NAME_DIR=@rpath)
    if [ ! -f "$P/lib/libvulkan.1.dylib" ]; then
        unpack Vulkan-Headers-$VK.tar.gz vkheaders
        cmake -S "$S/vkheaders" -B "$S/vkheaders/build" "${CM[@]}" && cmake --install "$S/vkheaders/build"
        unpack Vulkan-Loader-$VK.tar.gz vkloader
        # (SYSCONFDIR: its own system search folder, which would otherwise be this machine's build/deps/etc - a path
        # under the builder's home inside the shipped library)
        cmake -S "$S/vkloader" -B "$S/vkloader/build" "${CM[@]}" -DBUILD_TESTS=OFF -DSYSCONFDIR=/etc
        cmake --build "$S/vkloader/build" -j"$JOBS" && cmake --install "$S/vkloader/build"
    fi
    if [ ! -f "$P/lib/libshaderc_shared.1.dylib" ]; then
        unpack shaderc-2026.4.tar.gz shaderc
        unpack glslang-16.6.0.tar.gz shaderc/third_party/glslang
        unpack SPIRV-Tools-$VK.tar.gz shaderc/third_party/spirv-tools
        unpack SPIRV-Headers-$VK.tar.gz shaderc/third_party/spirv-headers
        cmake -S "$S/shaderc" -B "$S/shaderc/build" "${CM[@]}" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
              -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF
        cmake --build "$S/shaderc/build" -j"$JOBS" --target shaderc_shared
        mkdir -p "$P/include/shaderc"
        cp "$S"/shaderc/libshaderc/include/shaderc/*.h "$P/include/shaderc/"
        cp -P "$S"/shaderc/build/libshaderc/libshaderc_shared*.dylib "$P/lib/"
    fi
fi

if [ ! -x "$P/python/bin/python3.12" ]; then
    rm -rf "$P/python" && mkdir -p "$P/python"
    tar -xf "$S/cpython-3.12.14-20260924.tar.gz" -C "$P/python" --strip-components 1
fi

echo "build/deps ready (macOS $MIN):"
for f in "$P"/lib/*.dylib; do
    [ -L "$f" ] && continue
    printf '  %-28s minos %s\n' "$(basename "$f")" "$(otool -l "$f" | awk '/LC_BUILD_VERSION/ {b=1} b && /minos/ {print $2; exit}')"
done
