"""Exercise sampler replacement and alternating blend state with public synthetic inputs."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
nvn = root / "runtime/nvn"
objects = (nvn/"objects.c").read_text(encoding="utf8")
executor = (nvn/"vulkan/exec.c").read_text(encoding="utf8")


def section(source, first, following):
    start = source.index(first)
    return source[start:source.index(following, start)]


with tempfile.TemporaryDirectory(prefix="sw-sampler-") as folder:
    tmp = Path(folder)
    # Compile the same private lookup and public HLE entry points used by the runtime.
    registration = section(objects, "typedef struct {\n    uint64_t device;\n    uint8_t min_filter", "#define SD ")
    registration += section(objects, "static NvnSamplerDesc sampler_desc_from_builder(", "// An NVNsampler holds")
    registration += section(objects, "static void register_sampler(", "NvnDescPool *nvn_find_pool")
    (tmp/"registration.inc").write_text(registration, encoding="utf8")
    (tmp/"lookup.inc").write_text(section(executor, "static void texture_info(int stage", "\nstatic void texture_info_("), encoding="utf8")
    exe = tmp/("memo.exe" if os.name == "nt" else "memo")
    subprocess.run([os.environ.get("CC", "clang"), "-std=gnu11", "-O2", "-Wall", "-Werror",
                    *shlex.split(os.environ.get("CPPFLAGS","")), "-I"+str(root/"runtime"), "-I"+str(tmp),
                    str(root/"tools/test_texture_sampler_memo.c"), "-o", str(exe)], check=True)
    env = os.environ.copy()
    env.pop("SAVAGEWIND_VK_NO_TEX_MEMO",None)
    for api in ("sampler", "builder"):
        subprocess.run([str(exe), api], check=True, env=env)
    print("Sampler replacement, blend variants and unchanged registrations passed for both NVN APIs.")
