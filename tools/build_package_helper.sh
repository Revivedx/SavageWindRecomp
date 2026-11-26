#!/bin/bash
# Publish the NSP reader with its runtime so release users need no .NET installation.
set -euo pipefail
cd "$(dirname "$0")/.."
rid=${1:?target .NET runtime identifier}
out=${2:?output directory}
dotnet publish tools/nxpatch/nxpatch.csproj -c Release -r "$rid" --self-contained true \
    -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true \
    -p:IncludeNativeLibrariesForSelfExtract=true -p:DebugType=none \
    --artifacts-path build/nxpatch-artifacts -o "$out"
runtime="$(dotnet --list-runtimes | tr -d '\r' | sed -n 's/^Microsoft\.NETCore\.App 10\..* \[\(.*\)\]$/\1/p' | tail -1)"
: "${runtime:?.NET 10 runtime location unavailable}"
if command -v cygpath >/dev/null; then runtime="$(cygpath -u "$runtime")"; fi
cp "$runtime/../../LICENSE.txt" "$out/dotnet-LICENSE.txt"
cp "$runtime/../../ThirdPartyNotices.txt" "$out/dotnet-ThirdPartyNotices.txt"
