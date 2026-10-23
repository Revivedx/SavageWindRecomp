#!/bin/sh
# GPU test of the Vulkan shader path (tools/tests/test_vk_pipelines.c): fuzzed Maxwell programs -> GLSL -> the
# backend's compile/reflection/layouts -> real pipelines on the device, under the validation layer if present.
# usage: tools/tests/test_vk_pipelines.sh [count] [seed]      (Windows / MSYS2 ucrt64)
set -e
cd "$(dirname "$0")/../.."
OUT=build/test_vk_pipelines
mkdir -p "$OUT"
CC=${CC:-clang}
$CC -O2 -w -Iruntime -Iruntime/nvn -D_FILE_OFFSET_BITS=64 tools/tests/test_vk_pipelines.c runtime/nvn/maxwell/translate.c \
    runtime/nvn/vulkan/shaders.c -o "$OUT/test_vk_pipelines" -lvulkan-1 -lshaderc_shared
# the MSYS2 validation layer manifest lives next to its DLL
for d in /ucrt64/bin /c/msys64/ucrt64/bin; do
  [ -f "$d/VkLayer_khronos_validation.json" ] && export VK_ADD_LAYER_PATH="$(cygpath -w "$d")"
done
"$OUT/test_vk_pipelines" runtime/nvn/maxwell/translate.c "${1:-2000}" "${2:-1}"
