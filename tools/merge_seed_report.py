#!/usr/bin/env python3
"""Add the pipelines in players' problem reports to the shipped pipeline seed.

A problem report zip (Options > Problem report) carries the player's pipeline list and shader key log
(pipelines/vk_pipelines.list, pipelines/vk_shader_keys.list): hashes and render state, nothing of the game. This
turns each into a seed with savagewind --export-pipeline-seed (in a scratch cache folder, SAVAGEWIND_PSO_FOREIGN so a
list from another GPU is read, SAVAGEWIND_SEED_TRANSLATOR so its ids are worked out as the player's build named them)
and adds the records the seed doesn't have yet.

What it checks: the export keeps only pipelines whose shaders are in the game's own archives with the keys the
player's key log names, so a damaged or made-up list adds nothing. A list from a build with another file format
(record size, translation format) gives no records. Pipelines a GPU can't build are skipped by the pre-warm, as for
any seed.

    tools/merge_seed_report.py report1.zip [report2.zip ...] [--seed runtime/nvn/vulkan/vk_pipelines.seed]
        [--exe build/savagewind.exe] [--dry-run]

Run it from the checkout's root (the data root with extracted/, like the test scripts).
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

SEED_MAGIC = b"SWPSEED1"
SEED_HEADER = struct.Struct("<8s6I")  # magic, version, nrecs, key_size, state_size, zsize, reserved
LIST_HEADER = struct.Struct("<8s4I64s")  # magic, version, rec_size, vendor, device, shaders
LIST_FILES = ("vk_pipelines.list", "vk_shader_keys.list")
VENDORS = {0x10DE: "NVIDIA", 0x1002: "AMD", 0x8086: "Intel", 0x13B5: "Arm", 0x5143: "Qualcomm"}


def read_seed(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, nrecs, key_size, state_size, zsize, _ = SEED_HEADER.unpack_from(data)
    if magic != SEED_MAGIC or version != 1:
        raise ValueError(f"{path}: not a pipeline seed")
    raw = zlib.decompress(data[SEED_HEADER.size:SEED_HEADER.size + zsize])
    size = 4 + 16 + 2 * key_size + state_size
    if len(raw) != nrecs * size:
        raise ValueError(f"{path}: {len(raw)} bytes for {nrecs} records of {size}")
    return (key_size, state_size), [raw[i:i + size] for i in range(0, len(raw), size)]


def write_seed(path, layout, recs):
    z = zlib.compress(b"".join(recs), 9)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(SEED_HEADER.pack(SEED_MAGIC, 1, len(recs), layout[0], layout[1], len(z), 0))
        f.write(z)
    os.replace(tmp, path)


def report_seed(zip_path, exe, work):
    """The report's pipeline list as a seed file in work/, or (None, why)."""
    cache = os.path.join(work, "cache")
    os.makedirs(cache)
    with zipfile.ZipFile(zip_path) as z:
        names = set(z.namelist())
        for name in LIST_FILES:
            if "pipelines/" + name not in names:
                return None, "no pipeline list in it (made by an older build)"
            with z.open("pipelines/" + name) as src, open(os.path.join(cache, name), "wb") as dst:
                shutil.copyfileobj(src, dst)
    with open(os.path.join(cache, LIST_FILES[0]), "rb") as f:
        head = f.read(LIST_HEADER.size)
    if len(head) < LIST_HEADER.size:
        return None, "the pipeline list is empty"
    magic, version, rec_size, vendor, device, shaders = LIST_HEADER.unpack(head)
    if magic != b"SWPSOLST":
        return None, "the pipeline list is damaged"
    translator = shaders.split(b"\0")[0].decode("ascii", "replace").split("/")[0]
    gpu = f"{VENDORS.get(vendor, hex(vendor))} device {device:#x}"
    out = os.path.join(work, "report.seed")
    env = dict(os.environ, SAVAGEWIND_CACHE_DIR=cache, SAVAGEWIND_PSO_FOREIGN="1",
               SAVAGEWIND_SEED_TRANSLATOR=translator, SAVAGEWIND_HIDDEN="1")
    r = subprocess.run([exe, "--export-pipeline-seed", out], env=env, capture_output=True, text=True,
                       errors="replace", timeout=900)
    lines = [l for l in (r.stdout + r.stderr).splitlines() if "pipeline seed" in l or "pipeline list" in l]
    if r.returncode or not os.path.exists(out):
        return None, f"export failed ({gpu}, exit {r.returncode}): " + " | ".join(lines[-3:] or (r.stdout + r.stderr).splitlines()[-3:])
    return out, f"{gpu}, translator {translator}; " + " | ".join(l.split("] ", 1)[-1] for l in lines[-2:-1])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("reports", nargs="+", help="problem report zips")
    ap.add_argument("--seed", default="runtime/nvn/vulkan/vk_pipelines.seed")
    ap.add_argument("--exe", default="build/savagewind.exe" if os.name == "nt" else "build/savagewind")
    ap.add_argument("--dry-run", action="store_true", help="count the new pipelines, don't write the seed")
    a = ap.parse_args()
    layout, recs = read_seed(a.seed)
    known = set(recs)
    start = len(recs)
    print(f"seed {a.seed}: {start} pipelines")
    for rep in a.reports:
        work = tempfile.mkdtemp(prefix="seedrep-")
        try:
            path, info = report_seed(rep, os.path.abspath(a.exe), work)
            if not path:
                print(f"{rep}: nothing added: {info}")
                continue
            lay, got = read_seed(path)
            if lay != layout:
                print(f"{rep}: nothing added: record layout {lay} differs from the seed's {layout}")
                continue
            new = [r for r in got if r not in known]
            known.update(new)
            recs.extend(new)
            print(f"{rep}: {len(got)} pipelines, {len(new)} new ({info})")
        finally:
            shutil.rmtree(work, ignore_errors=True)
    print(f"total: {len(recs)} pipelines, +{len(recs) - start}")
    if not a.dry_run and len(recs) > start:
        write_seed(a.seed, layout, recs)
        print(f"written {a.seed} ({os.path.getsize(a.seed) / 1024:.1f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
