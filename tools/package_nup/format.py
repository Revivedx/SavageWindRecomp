"""NUP (NX Package) format constants and binary layout.

Implements SPEC.md v1 exactly. All integers little-endian. All reserved
fields must be zero on write and are validated on read.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum, auto
import struct
from typing import Final, TypedDict

MAGIC: Final = b"NUP\x01"  # 'N','U','P', format major version 1
TOC_MAGIC: Final = b"TOC\x01"
HEADER_SIZE: Final = 0x80
PKG_SIZE: Final = 0x40
DIR_SIZE: Final = 0x18
FILE_SIZE: Final = 0x50
EXTENT_ALIGN: Final = 16
NO_DIR: Final = 0xFFFFFFFF

class Codec(IntEnum):
    RAW = 0
    ZSTD = auto()

CODEC_RAW: Final = Codec.RAW
CODEC_ZSTD: Final = Codec.ZSTD
assert len(Codec) == 2

class Kind(IntEnum):
    BASE = 0
    UPDATE = auto()
    ADDON = auto()
    ADDON_UPDATE = auto()
    SYSTEM = auto()

KIND_BASE: Final = Kind.BASE
KIND_UPDATE: Final = Kind.UPDATE
KIND_ADDON: Final = Kind.ADDON
KIND_ADDON_UPDATE: Final = Kind.ADDON_UPDATE
KIND_SYSTEM: Final = Kind.SYSTEM
assert len(Kind) == 5

KIND_NAMES: Final = {
    KIND_BASE: "base",
    KIND_UPDATE: "update",
    KIND_ADDON: "addon",
    KIND_ADDON_UPDATE: "addon_update",
    KIND_SYSTEM: "system",
}

# Meta TLV types (SPEC §5)
class TVL(IntEnum):
    NPDM = 0
    NACP = auto()
    ICON = auto()
    DISPLAY_VERSION = auto()
    TITLE_NAME = auto()
    PUBLISHER = auto()
    BUILD_ID = auto()
    SOURCE_DUMP = auto()
    LOCALIZED_TITLES = auto()
    LOCALIZED_ICONS = auto()

TLV_NPDM: Final = TVL.NPDM
TLV_NACP: Final = TVL.NACP
TLV_ICON: Final = TVL.ICON
TLV_DISPLAY_VERSION: Final = TVL.DISPLAY_VERSION
TLV_TITLE_NAME: Final = TVL.TITLE_NAME
TLV_PUBLISHER: Final = TVL.PUBLISHER
TLV_BUILD_ID: Final = TVL.BUILD_ID
TLV_SOURCE_DUMP: Final = TVL.SOURCE_DUMP
TLV_LOCALIZED_TITLES: Final = TVL.LOCALIZED_TITLES
TLV_LOCALIZED_ICONS: Final = TVL.LOCALIZED_ICONS
assert len(TVL) == 10

PKG_FLAG_STANDALONE_UPDATE: Final = 0x01

_super_s: Final = struct.Struct("<4sHHQQ32sQQII16s32s")  # 128
_pkg_s: Final = struct.Struct("<QIIIIBBHIQI20x")  # 64
_dir_s: Final = struct.Struct("<IIIIQ")  # 24
_file_s: Final = struct.Struct("<IIQQQ32sBBH12x")  # 80
_toc_head_s: Final = struct.Struct("<4sIQQQQQ")  # 48
_tlv_s: Final = struct.Struct("<HHI")  # 8

# struct sizes must match the documented record sizes exactly (SPEC §8);
assert _super_s.size == HEADER_SIZE
assert _pkg_s.size == PKG_SIZE
assert _dir_s.size == DIR_SIZE
assert _file_s.size == FILE_SIZE


class NupError(Exception):
    """Malformed or nonconforming NUP data (fail closed)."""


@dataclass
class Package:
    title_id: int = 0
    root_dir: int = 0
    exefs_dir: int = NO_DIR
    romfs_dir: int = NO_DIR
    version: int = 0
    kind: int = KIND_BASE
    flags: int = 0
    meta_offset: int = 0
    meta_size: int = 0


@dataclass
class DirNode:
    name: bytes
    first_child: int = 0
    child_count: int = 0


@dataclass
class FileNode:
    name: bytes
    size: int = 0
    sha256: bytes = b"\0" * 32
    extent_offset: int = 0
    extent_size: int = 0
    codec: int = CODEC_RAW
    codec_flags: int = 0
    # pack-time only (not serialized)
    src_path: str | None = field(default=None, repr=False)


class Super(TypedDict):
    header_size: int
    header_version: int
    toc_offset: int
    toc_size: int
    toc_sha256: bytes
    blob_offset: int
    file_size: int
    package_count: int
    flags: int
    uuid: bytes


def pack_super(magic: bytes, header_size: int, header_version: int, toc_offset: int,
               toc_size: int, toc_sha256: bytes, blob_offset: int, file_size: int,
               package_count: int, flags: int, uuid: bytes) -> bytes:
    assert len(toc_sha256) == 32 and len(uuid) == 16
    return _super_s.pack(magic, header_size, header_version, toc_offset, toc_size,
                         toc_sha256, blob_offset, file_size, package_count, flags, uuid,
                         b"\0" * 32)


def pack_pkg(p: Package) -> bytes:
    return _pkg_s.pack(p.title_id, p.root_dir, p.exefs_dir, p.romfs_dir, p.version,
                       p.kind, p.flags, 0, 0, p.meta_offset, p.meta_size)


def pack_dir(d: DirNode) -> bytes:
    return _dir_s.pack(0, 0, d.first_child, d.child_count, 0)  # name off/len patched at write


def pack_file(f: FileNode) -> bytes:
    return _file_s.pack(0, 0, f.size, f.extent_offset, f.extent_size, f.sha256,
                        f.codec, f.codec_flags, 0)


def parse_super(buf: bytes) -> Super:
    if len(buf) < HEADER_SIZE:
        raise NupError(f"superblock truncated: {len(buf)} < {HEADER_SIZE}")
    (magic, header_size, header_version, toc_offset, toc_size, toc_sha256,
     blob_offset, file_size, package_count, flags, uuid, reserved) = _super_s.unpack_from(buf, 0)
    if magic[0:3] != MAGIC[0:3]:
        raise NupError(f"bad magic {magic!r}, expected {MAGIC[0:3]!r}")
    if magic[3] != 1:
        raise NupError(f"unsupported format major version {magic[3]}")
    if header_size < HEADER_SIZE:
        raise NupError(f"header_size {header_size:#x} < {HEADER_SIZE:#x}")
    if reserved != b"\0" * 32:
        raise NupError("superblock reserved bytes not zero")
    if flags != 0:
        raise NupError(f"unknown superblock flags {flags:#x}")
    return Super(header_size=header_size, header_version=header_version,
                 toc_offset=toc_offset, toc_size=toc_size, toc_sha256=toc_sha256,
                 blob_offset=blob_offset, file_size=file_size,
                 package_count=package_count, flags=flags, uuid=uuid)


def parse_pkg(buf: bytes, off: int) -> Package:
    (title_id, root, exefs, romfs, version, kind, flags, _r, _r2,
     meta_offset, meta_size) = _pkg_s.unpack_from(buf, off)
    if kind >= len(KIND_NAMES):
        raise NupError(f"unknown package kind {kind}")
    if _r != 0 or _r2 != 0:
        raise NupError("package reserved bytes not zero")
    return Package(title_id=title_id, root_dir=root, exefs_dir=exefs, romfs_dir=romfs,
                   version=version, kind=kind, flags=flags,
                   meta_offset=meta_offset, meta_size=meta_size)


def parse_file(toc: bytes, pos: int) -> tuple[tuple[int, int], int, bytes, int, int, Codec]:
    (off, ln, size, eoff, esz, sha, codec, cflags, _r4) = \
        _file_s.unpack_from(toc, pos)
    pos += FILE_SIZE
    try:
        codec = Codec(codec)
    except ValueError as error:
        raise NupError(f"unknown extent codec {codec}") from error
    if cflags != 0:
        raise NupError(f"unknown codec_flags {cflags}")
    if size == 0 and (eoff != 0 or esz != 0 or codec != CODEC_RAW):
        raise NupError("zero-size file with nonempty extent")
    if size != 0 and esz == 0:
        raise NupError(f"file with nonzero size but empty extent")
    return ((off, ln), size, sha, eoff, esz, codec)
