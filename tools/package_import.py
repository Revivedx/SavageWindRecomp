"""Extract selected BOTW packages into an empty launcher-owned directory."""

import argparse
from contextlib import ExitStack
from dataclasses import dataclass
import hashlib
import io
from pathlib import Path, PureWindowsPath
from queue import Empty, SimpleQueue
import re
import shutil
import struct
import subprocess
import sys
from threading import Thread
import time

from package_nup import format as nup
from package_nup.read import NupReader, PackageInfo


BASE = 0x01007EF00011E000
UPDATE = BASE + 0x800
DLC = {BASE + 0x1001: "aoc1", BASE + 0x1002: "aoc2"}
BLOCK = 1024 * 1024


class PackageError(Exception):
    pass


@dataclass(frozen=True)
class Content:
    path: Path
    kind: str
    title: int
    version: int
    reader: NupReader | None = None
    package: PackageInfo | None = None


class Extent(io.RawIOBase):
    def __init__(self, archive, start, length):
        self.archive = archive
        self.offset = start
        self.end = start + length

    def readable(self):
        return True

    def readinto(self, target):
        count = min(len(target), self.end - self.offset)
        target[:count] = self.archive[self.offset:self.offset + count]
        self.offset += count
        return count


def blocks(reader, index):
    _, size, digest, offset, stored, codec = reader.file_info(index)
    if codec == nup.CODEC_RAW and stored != size:
        raise PackageError("invalid")
    if not size and (offset or stored or codec != nup.CODEC_RAW):
        raise PackageError("invalid")

    source = io.BufferedReader(Extent(reader._mm, offset, stored), BLOCK)
    with ExitStack() as stack:
        stack.enter_context(source)
        if codec == nup.CODEC_ZSTD:
            import zstandard
            frame = zstandard.get_frame_parameters(source.peek(18))
            if frame.content_size != size or frame.window_size > 128 * BLOCK:
                raise PackageError("invalid")
            source = stack.enter_context(
                zstandard.ZstdDecompressor()
                .stream_reader(source, read_across_frames=True))
        hashed = hashlib.sha256()
        remaining = size
        while data := source.read(min(BLOCK, remaining + 1)):
            remaining -= len(data)
            if remaining < 0:
                raise PackageError("invalid")
            hashed.update(data)
            yield data
        if remaining or hashed.digest() != digest:
            raise PackageError("invalid")


def members(reader, root, folder):
    pending = [(root, folder)]
    visited = set()
    while pending:
        directory, target = pending.pop()
        if directory in visited:
            raise PackageError("invalid")
        visited.add(directory)
        _, first, count = reader.dirs[directory]
        names = set()
        for node in reader.children[first:first + count]:
            is_dir = node < reader.dir_count
            index = node if is_dir else node - reader.dir_count
            if not is_dir and index >= reader.file_count:
                raise PackageError("invalid")
            name = (reader.dirs if is_dir else reader.files)[index][0].decode("utf-8")
            if (not name or name.endswith((".", " ")) or PureWindowsPath(name).is_reserved()
                    or any(ch in '<>:"/\\|?*' or ord(ch) < 32 for ch in name)
                    or name.casefold() in names):
                raise PackageError("invalid")
            names.add(name.casefold())
            if is_dir:
                pending.append((index, target / name))
            else:
                yield target / name, index


def run_nxpatch(args, command, options):
    argv = [str(args.nxpatch), command, "-k", str(args.keys)]
    if command == "extract":
        argv += ["--free", str(shutil.disk_usage(args.out).free)]
    titlekeys = args.keys.with_name("title.keys")
    if titlekeys.is_file():
        argv += ["--titlekeys", str(titlekeys)]
    # Capture the protocol only. Helper diagnostics can contain player paths.
    errors = set()
    records = []
    with subprocess.Popen(argv + options, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          encoding="utf-8", errors="replace") as child:
        lines = SimpleQueue()

        def read_lines():
            try:
                for line in child.stdout:
                    lines.put(line)
            finally:
                lines.put(None)

        reader = Thread(target=read_lines, daemon=True)
        reader.start()
        while True:
            if args.cancel.exists():
                child.kill()
                reader.join()
                raise PackageError("cancelled")
            try:
                line = lines.get(timeout=0.1)
            except Empty:
                continue
            if line is None:
                break
            fields = line.rstrip("\r\n").split("\t")
            if fields[0] == "content" and len(fields) == 4:
                records.append(fields[1:])
            elif fields[0] == "version" and len(fields) == 3:
                records.append(fields[1:])
            elif fields[0] == "nxpatch-error" and len(fields) >= 2:
                errors.add(fields[1])
            elif fields[0] == "error":
                errors.add(fields[1] if len(fields) >= 2 else "invalid")
            elif fields[0] in ("total", "progress") and len(fields) == 2 and fields[1].isdigit():
                if command == "extract":
                    print(line.rstrip(), flush=True)
        if child.wait() or errors:
            if errors & {"missing_key", "missing_title_key"}:
                raise PackageError("keys")
            if "bad_game_version" in errors:
                raise PackageError("version")
            raise PackageError("space" if "disk_space" in errors else "invalid")
    return records


def inventory(args, stack):
    for path in dict.fromkeys(path.resolve(strict=True) for path in args.packages):
        if args.cancel.exists():
            raise PackageError("cancelled")
        if path.suffix.lower() == ".nup":
            reader = stack.enter_context(NupReader(path))
            for p in reader.packages:
                yield Content(path, {"addon": "dlc"}.get(p.kind_name, p.kind_name),
                              p.title_id, p.version, reader, p)
        elif path.suffix.lower() == ".nsp":
            if args.nxpatch is None or not args.nxpatch.is_file():
                raise PackageError("helper")
            if args.keys is None or not args.keys.is_file():
                raise PackageError("keys")
            for kind, title, version in run_nxpatch(args, "identify", [str(path)]):
                yield Content(path, kind, int(title, 16), int(version))
        else:
            raise PackageError("invalid")


def newest(entries):
    ordered = sorted(entries, key=lambda e: -e.version)
    if len(ordered) > 1 and ordered[0].version == ordered[1].version:
        raise PackageError("conflict")
    return ordered[0] if ordered else None


def display_version(value):
    value = value.split("\0", 1)[0].strip()
    return value if re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", value) else ""


def nup_version(entry, *, allow_metadata=True):
    reader, package = entry.reader, entry.package

    def child(directory, name, is_dir):
        if directory == nup.NO_DIR:
            return nup.NO_DIR
        _, start, count = reader.dirs[directory]
        for node in reader.children[start:start + count]:
            if node < reader.dir_count:
                if is_dir and reader.dirs[node][0] == name:
                    return node
            elif not is_dir:
                index = node - reader.dir_count
                if index < reader.file_count and reader.file_info(index)[0] == name:
                    return index
        return nup.NO_DIR

    system = child(package.romfs_dir, b"System", True)
    index = child(system, b"AocVersion.txt" if entry.kind == "dlc" else b"Version.txt", False)
    if index != nup.NO_DIR and reader.file_info(index)[1] <= 64:
        version = display_version(b"".join(blocks(reader, index)).decode("utf-8-sig", errors="replace"))
        if version:
            return version
    if not allow_metadata or entry.kind == "base":
        # Some writers put the bundle's updated display version on the base record.
        return ""
    # The published spec and older reference writers use different TLV numbers.
    start, end = package.meta_offset, package.meta_offset + package.meta_size
    while start + nup._tlv_s.size <= end:
        kind, _, size = nup._tlv_s.unpack_from(reader._mm, start)
        start += nup._tlv_s.size
        if start + size > end:
            break
        if kind in (4, nup.TLV_DISPLAY_VERSION) and size <= 64:
            version = display_version(reader._mm[start:start + size].decode("utf-8", errors="replace"))
            if version:
                return version
        start += size
    return ""


def review(args, selected, nsp_versions):
    if args.confirm is None:
        return
    for entry in selected:
        kind = DLC[entry.title] if entry.kind == "dlc" else entry.kind
        name = "".join(ch if ord(ch) >= 32 else " " for ch in entry.path.name)
        version = nup_version(entry) if entry.reader else display_version(nsp_versions.get(kind, ""))
        print(f"item\t{kind}\t{version}\t{name}", flush=True)
    print("review", flush=True)
    while not args.confirm.exists():
        if args.cancel.exists():
            raise PackageError("cancelled")
        time.sleep(0.1)
    if args.cancel.exists():
        raise PackageError("cancelled")


def extract(args, entries):
    base = [e for e in entries if e.kind == "base" and e.title == BASE]
    # Some NUP producers record the application ID on the update instead of the patch ID.
    updates = [e for e in entries if e.kind == "update" and
               (e.title == UPDATE or (e.reader and e.title == BASE))]
    addons = [e for e in entries if e.kind == "dlc" and e.title in DLC]
    if not base and not updates and not addons:
        raise PackageError("title")

    game = newest(updates) or newest(base)
    selected = [game] if game else []
    nsp_options = []
    if game and not game.reader:
        if game.kind != "update":
            raise PackageError("update")
        nsp_base = newest(e for e in base if e.reader is None)
        if not nsp_base:
            raise PackageError("base")
        nsp_options = ["--base", str(nsp_base.path), "--update", str(game.path),
                       "--version", str(game.version)]
        selected.insert(0, nsp_base)
    elif game:
        if nup_version(game, allow_metadata=False) not in args.game_version:
            raise PackageError("version")
        if game.kind == "update" and len(base) == 1:
            selected.insert(0, base[0])

    tasks = []
    if game and game.reader:
        tasks.extend((game.reader, section, args.out / name) for name, section in
                     (("exefs", game.package.exefs_dir), ("romfs", game.package.romfs_dir))
                     if section != nup.NO_DIR)
    for title, name in DLC.items():
        entry = newest(e for e in addons if e.title == title)
        if not entry:
            continue
        selected.append(entry)
        if entry.reader:
            if entry.package.romfs_dir == nup.NO_DIR:
                raise PackageError("invalid")
            tasks.append((entry.reader, entry.package.romfs_dir, args.out / name))
        else:
            nsp_options += ["--dlc", f"{title:X}|{entry.version}|{entry.path}"]

    files = [(reader, path, index) for reader, root, target in tasks
             for path, index in members(reader, root, target)]
    total = sum(reader.file_info(index)[1] for reader, _, index in files)
    if shutil.disk_usage(args.out).free < total:
        raise PackageError("space")
    if not files and not nsp_options:
        raise PackageError("invalid")
    nsp_versions = {}
    if nsp_options and (args.confirm is not None or (game and not game.reader)):
        nsp_versions = dict(run_nxpatch(args, "extract", nsp_options +
                                       ["--out", str(args.out), "--versions", "1"]))
    if game and not game.reader:
        merged_version = nsp_versions.get("update")
        if merged_version not in args.game_version:
            raise PackageError("version")
        nsp_options += ["--require-version", merged_version]
    review(args, selected, nsp_versions)
    if files:
        print(f"total\t{total}", flush=True)
    done = 0
    for reader, path, index in files:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as output:
            for chunk in blocks(reader, index):
                if args.cancel.exists():
                    raise PackageError("cancelled")
                output.write(chunk)
                done += len(chunk)
        if done // (64 * BLOCK) != (done - reader.file_info(index)[1]) // (64 * BLOCK):
            print(f"progress\t{done}", flush=True)
    if files:
        print(f"progress\t{done}", flush=True)
    if nsp_options:
        run_nxpatch(args, "extract", nsp_options + ["--out", str(args.out)])


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cancel", type=Path, required=True)
    parser.add_argument("--confirm", type=Path)
    parser.add_argument("--keys", type=Path)
    parser.add_argument("--nxpatch", type=Path)
    parser.add_argument("--game-version", action="append", required=True)
    parser.add_argument("packages", type=Path, nargs="+")
    args = parser.parse_args(argv)
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    try:
        if not args.out.is_dir() or any(args.out.iterdir()):
            raise PackageError("invalid")
        with ExitStack() as stack:
            extract(args, list(inventory(args, stack)))
        return 0
    except PackageError as error:
        print(f"failure\t{error}", flush=True)
    except (OSError, ValueError, struct.error, nup.NupError):
        print("failure\tinvalid", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
