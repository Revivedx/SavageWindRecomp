#!/bin/bash
# usage: tools/packaging/zig_slim.sh <game code flags>: the Windows release's compiler, build/zig-slim/ (shipped as zig/), made
# from tools/packaging/fetch_zig.sh's build/zig/. The full zig compiles the MinGW C runtime and its own runtime library from
# ~4000 source files on the player's machine at the first link; here they are built once, ahead of time, and only
# what building the game code reads is kept:
#   zig.exe, lib/std/std.zig (how zig finds its lib folder), the headers the game code includes (from the compiler's
#   dependency lists of a probe), rt/ = the runtime libraries a `zig cc -shared` link uses and rt/link.rsp = that
#   link's flags and libraries (runtime/gamebuild.c links with `zig lld-link @rt/link.rsp -LIBPATH:rt`).
# Checked at the end: a probe compiled and linked with the slim copy and an empty cache builds nothing from source.
set -euo pipefail
cd "$(dirname "$0")/../.."
Z=build/zig O=build/zig-slim W=build/zig-slim-work OA=$PWD/build/zig-slim
[ -x $Z/zig.exe ] || { echo "tools/packaging/fetch_zig.sh first (the compiler)"; exit 1; }
rm -rf $O $W
mkdir -p $W/cache $O/lib/std $O/rt
export ZIG_GLOBAL_CACHE_DIR="$(cygpath -w "$PWD/$W/cache")" ZIG_LOCAL_CACHE_DIR="$(cygpath -w "$PWD/$W/cache")"
mkdir -p $W/tmp
export TMPDIR="$(cygpath -w "$PWD/$W/tmp")" TMP="$(cygpath -w "$PWD/$W/tmp")" TEMP="$(cygpath -w "$PWD/$W/tmp")"  # (MSYS's /c/... TMPDIR: not for zig)
flags=()  # (the game code's flags as gamebuild.c add_cflags passes them to zig)
for f in "$@"; do
    case $f in -march=*) c=${f#-march=}; flags+=("-mcpu=${c//-/_}") ;; *) flags+=("$f") ;; esac
done

# 1. the headers: what the generated C (cpu.h, simd.h; setjmp.h where it calls the guest setjmp: tools/recomp/gen.py
#    setjmp_call) and tools/recomp/gamelib_entry.c include
printf '#include "cpu.h"\n#include "simd.h"\n#include <setjmp.h>\n' > $W/probe.c
$Z/zig.exe cc "${flags[@]}" -Iruntime -c $W/probe.c -o $W/probe.o -MD -MF $W/probe.d
$Z/zig.exe cc "${flags[@]}" -Iruntime '-DGAMELIB_ID="0"' -c tools/recomp/gamelib_entry.c -o $W/entry.o -MD -MF $W/entry.d
n=0
for h in $(cat $W/probe.d $W/entry.d | tr ' ' '\n' | tr -d '\r' | sed -n 's|^build/zig/lib/|lib/|p' | sort -u); do
    mkdir -p "$O/$(dirname "$h")"
    cp "$Z/$h" "$O/$h"
    n=$((n + 1))
done
cp $Z/zig.exe $Z/LICENSE $O/
cp $Z/lib/std/std.zig $O/lib/std/

# 2. the runtime: a probe DLL importing from the exe, linked the way the full zig does it (-v: its lld-link line)
printf 'LIBRARY savagewind.exe\nEXPORTS\nprobe_host\n' > $W/host.def
printf '#include <setjmp.h>\nvoid probe_host(void);\nstatic jmp_buf jb;\n__declspec(dllexport) void probe(void) { if (!setjmp(jb)) probe_host(); }\n' > $W/dll.c
ZA=$PWD/$Z/zig.exe
(cd $W && $ZA dlltool -d host.def -l host.a -m i386:x86-64 && $ZA cc "${flags[@]}" -c dll.c -o dll.o &&
    $ZA cc -shared -o probe.dll dll.o host.a -v 2> link.log) || { echo "tools/packaging/zig_slim.sh: probe link failed ($W/link.log)"; exit 1; }
line=$(grep '^lld-link ' $W/link.log | tail -1 | tr -d '\r')
[ -n "$line" ] || { echo "tools/packaging/zig_slim.sh: no lld-link line in $W/link.log"; exit 1; }
: > $O/rt/link.rsp
for t in $line; do
    case $t in
        lld-link) ;;
        -OUT:*|-PDB:*|-PDBALTPATH:*|-DEBUG|-IMPLIB:*) ;;  # (gamebuild.c: -OUT; no debug info, no import library)
        -*) echo "$t" >> $O/rt/link.rsp ;;
        *ubsan_rt*) ;;  # (the sanitizer runtime of an unoptimized link: the game code is built with -O1)
        *.lib|*.obj)
            if [[ $t == *cache* ]]; then  # (paths relative to the folder the probe linked in)
                p=$(cygpath -u "$t"); [[ $p == /* ]] || p=$W/$p
                cp "$p" $O/rt/
                basename "$p" >> $O/rt/link.rsp
            fi ;;
    esac
done
echo "-NOIMPLIB" >> $O/rt/link.rsp

# 3. check: the probe again with the slim copy and an empty cache
rm -rf $W/cache2
export ZIG_GLOBAL_CACHE_DIR="$(cygpath -w "$PWD/$W/cache2")" ZIG_LOCAL_CACHE_DIR="$(cygpath -w "$PWD/$W/cache2")"
$O/zig.exe cc "${flags[@]}" -Iruntime -c $W/probe.c -o $W/probe2.o
$O/zig.exe cc "${flags[@]}" -Iruntime '-DGAMELIB_ID="0"' -c tools/recomp/gamelib_entry.c -o $W/entry2.o
(cd $W && "$OA/zig.exe" lld-link "@$(cygpath -w "$OA/rt/link.rsp")" "-LIBPATH:$(cygpath -w "$OA/rt")" \
    -OUT:probe2.dll dll.o host.a) || { echo "tools/packaging/zig_slim.sh: the slim zig's link failed"; exit 1; }
built=$(find $W/cache2 -name '*.lib' | wc -l)
[ "$built" = 0 ] || { echo "tools/packaging/zig_slim.sh: the slim zig still built $built libraries"; exit 1; }
echo "build/zig-slim: $n headers, $(ls $O/rt | wc -l) runtime files, $(find $O -type f | wc -l) files, $(du -sh $O | cut -f1)"
