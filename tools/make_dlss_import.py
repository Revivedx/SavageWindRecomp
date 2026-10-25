"""Build a MinGW import library for NVIDIA's Vulkan NGX runtime DLL.

The NVIDIA SDK ships an MSVC static wrapper library. Savage Wind is built with
clang/LLD and can link directly to the redistributable DLL instead, avoiding a
Visual Studio CRT dependency while keeping the SDK headers and API checks.
"""
from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_dlss_import.py <nvngx_dlss.dll> <output.a>", file=sys.stderr)
        return 2
    dll = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2])
    objdump = "objdump"
    dlltool = "dlltool"
    text = subprocess.check_output([objdump, "-p", str(dll)], text=True, errors="replace")
    names = sorted(
        {
            m.group(1)
            for line in text.splitlines()
            if (m := re.match(r"\s*\[\s*\d+\]\s+\+base\[\s*\d+\]\s+\S+\s+(NGX_[A-Za-z0-9_]+|NVSDK_NGX_[A-Za-z0-9_]+)\s*$", line))
        }
    )
    # The SDK's static wrapper defines these convenience entry points itself;
    # importing them from the runtime DLL would create duplicate symbols.
    names = [
        n
        for n in names
        if n not in {
            "NVSDK_NGX_VULKAN_CreateFeature",
            "NVSDK_NGX_VULKAN_CreateFeature1",
            "NVSDK_NGX_VULKAN_ReleaseFeature",
            "NVSDK_NGX_VULKAN_Shutdown1",
        }
    ]
    if not names:
        raise RuntimeError(f"no NGX exports found in {dll}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        definition = pathlib.Path(td) / "nvngx_dlss.def"
        definition.write_text("LIBRARY nvngx_dlss.dll\nEXPORTS\n" + "".join(f"  {n}\n" for n in names), encoding="ascii")
        subprocess.check_call([dlltool, "-d", str(definition), "-D", "nvngx_dlss.dll", "-l", str(output)])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
