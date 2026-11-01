#!/usr/bin/env python3
"""Convert Breath of the Wild save files between Wii U (Cemu) and Switch (Savage Wind).

    python tools/botw_save_convert.py --src <Cemu user/80000001 folder> --dst save/save_0

A BotW profile is a folder of small binary files. They have the same flag data on both consoles, but the Wii U is
big endian, the Switch little endian, and each game revision has its own flag list. This tool does not guess:
it reads the flag table (name, type, length, default) out of YOUR game's own `Pack/Bootup.pack`, then rebuilds
every save file for that exact revision. Nothing from the game is embedded in this script.

What is converted (see docs/save-converter.md for the format):
  <slot>/game_data.sav   the main save (flags: progress, inventory, map markers, position, ...)
  <slot>/caption.sav     the metadata the load screen shows
  <slot>/caption.jpg     copied as is (a plain JPEG)
  option.sav             settings; only flags that exist on both sides are kept
  tracker/               Hero's Path recording, byte-swapped (--tracker skip leaves it out, copy copies it as is)
  album/, pict_book/     photos and compendium pictures, copied as is

Existing files in the destination are never overwritten: with --overwrite they are renamed to <name>.bak first.
"""
import argparse
import os
import shutil
import struct
import sys
import time
import zlib

# ---------------------------------------------------------------------------------------------------------
# Minimal readers for the containers in Bootup.pack: SARC (archive), Yaz0 (compression), BYML (binary YAML).
# Only what this tool needs; read-only.
# ---------------------------------------------------------------------------------------------------------


def sarc_files(d):
    """{name: bytes} of a SARC archive."""
    if d[:4] != b'SARC':
        raise ValueError('not a SARC archive')
    e = '<' if d[6:8] == b'\xff\xfe' else '>'
    data_off, = struct.unpack_from(e + 'I', d, 0x0C)
    if d[0x14:0x18] != b'SFAT':
        raise ValueError('SARC: SFAT missing')
    nodes, = struct.unpack_from(e + 'H', d, 0x1A)
    sfnt = 0x20 + nodes * 16
    if d[sfnt:sfnt + 4] != b'SFNT':
        raise ValueError('SARC: SFNT missing')
    names = sfnt + 8
    out = {}
    for i in range(nodes):
        h, attr, start, end = struct.unpack_from(e + 'IIII', d, 0x20 + i * 16)
        name = hex(h)
        if attr >> 24:
            o = names + (attr & 0xFFFFFF) * 4
            name = d[o:d.index(b'\0', o)].decode()
        out[name.lstrip('/')] = d[data_off + start:data_off + end]
    return out


def yaz0_decompress(d):
    if d[:4] != b'Yaz0':
        return d
    size, = struct.unpack_from('>I', d, 4)
    out = bytearray()
    i = 16
    while len(out) < size:
        code = d[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(d[i])
                i += 1
            else:
                b1, b2 = d[i], d[i + 1]
                i += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = d[i] + 0x12
                    i += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


class Byml:
    """BYML v2/v3 reader (the flag tables only use hash, array, string, bool, int, uint, float)."""

    def __init__(self, d):
        self.d = d
        if d[:2] == b'YB':
            self.little = True
        elif d[:2] == b'BY':
            self.little = False
        else:
            raise ValueError('not a BYML file')
        self.e = '<' if self.little else '>'
        _, name_tab, str_tab, self.root = struct.unpack_from(self.e + 'HIII', d, 2)
        self.names = self._strings(name_tab) if name_tab else []
        self.strs = self._strings(str_tab) if str_tab else []

    def _u(self, o, n=4):
        return int.from_bytes(self.d[o:o + n], 'little' if self.little else 'big')

    def _count(self, o):
        # node header: 1 byte type, 3 byte count (same byte order as the file)
        return int.from_bytes(self.d[o + 1:o + 4], 'little' if self.little else 'big')

    def _strings(self, o):
        n = self._count(o)
        offs = [self._u(o + 4 + 4 * i) for i in range(n)]
        return [self.d[o + a:self.d.index(b'\0', o + a)].decode('utf8', 'replace') for a in offs]

    def _value(self, t, v):
        e = self.e
        if t == 0xA0:
            return self.strs[v]
        if t == 0xD0:
            return bool(v)
        if t == 0xD1:
            return struct.unpack(e + 'i', struct.pack(e + 'I', v))[0]
        if t == 0xD2:
            return struct.unpack(e + 'f', struct.pack(e + 'I', v))[0]
        if t == 0xD3:
            return v
        if t == 0xD4:
            return struct.unpack_from(e + 'q', self.d, v)[0]
        if t == 0xD5:
            return struct.unpack_from(e + 'Q', self.d, v)[0]
        if t == 0xD6:
            return struct.unpack_from(e + 'd', self.d, v)[0]
        if t == 0xFF:
            return None
        if t in (0xC0, 0xC1):
            return self._node(v)
        raise ValueError('BYML: unsupported node type 0x%X' % t)

    def _node(self, o):
        t, n = self.d[o], self._count(o)
        if t == 0xC0:
            types = self.d[o + 4:o + 4 + n]
            base = o + 4 + ((n + 3) & ~3)
            return [self._value(types[i], self._u(base + 4 * i)) for i in range(n)]
        if t == 0xC1:
            out = {}
            for i in range(n):
                a = o + 4 + 8 * i
                out[self.names[self._u(a, 3)]] = self._value(self.d[a + 3], self._u(a + 4))
            return out
        raise ValueError('BYML: container expected, got 0x%X' % t)

    def parse(self):
        return self._node(self.root)


# ---------------------------------------------------------------------------------------------------------
# The flag table of one game revision
# ---------------------------------------------------------------------------------------------------------

# bytes per element of each flag type, in 32-bit words
_WORDS = {'bool': 1, 's32': 1, 'f32': 1, 'vector2f': 2, 'vector3f': 3, 'vector4f': 4,
          'string': 8, 'string64': 16, 'string256': 64}


class Flag:
    __slots__ = ('name', 'hash', 'kind', 'words', 'init')

    def __init__(self, name, h, kind, words, init):
        self.name, self.hash, self.kind, self.words, self.init = name, h, kind, words, init

    @property
    def is_string(self):
        return self.kind.startswith('string')


def _flatten(v):
    if isinstance(v, (list, tuple)):
        for x in v:
            yield from _flatten(x)
    elif isinstance(v, dict):
        yield from _flatten(v['Values'])
    else:
        yield v


def _encode_init(kind, init, words):
    """Default words for a flag (little-endian values; strings as raw bytes), `words` 32-bit words long."""
    base = kind.replace('_array_data', '').replace('_data', '')
    vals = list(_flatten(init))
    if base.startswith('string'):
        size = _WORDS[base] * 4
        raw = b''.join(str(s).encode('utf8')[:size - 1].ljust(size, b'\0') for s in vals)
        return raw.ljust(words * 4, b'\0')[:words * 4]
    if base == 'bool':
        # bool flags carry unrelated numbers in InitValue (respawn data); only a literal 1 means "set"
        raw = b''.join(struct.pack('<I', 1 if x == 1 else 0) for x in vals)
    elif base == 'f32' or base.startswith('vector'):
        raw = b''.join(struct.pack('<f', float(x)) for x in vals)
    else:
        raw = b''.join(struct.pack('<I', int(x) & 0xFFFFFFFF) for x in vals)
    return raw.ljust(words * 4, b'\0')[:words * 4]


class Format:
    """Everything needed to read or write the save files of one game revision."""

    def __init__(self, bootup_path):
        pack = sarc_files(_read(bootup_path))
        self.revision = None
        self.files = {}      # 'game_data.sav' / 'caption.sav' / 'option.sav' -> sorted list of hashes
        meta = self._load_flags(sarc_files(yaz0_decompress(pack['GameData/gamedata.ssarc'])))
        fmt = sarc_files(yaz0_decompress(pack['GameData/savedataformat.ssarc']))
        per_file = {}
        for name, blob in fmt.items():
            if not name.endswith('.bgsvdata'):
                continue
            doc = Byml(blob).parse()
            self.revision = doc['save_info'][0]['revision']
            desc, flags = doc['file_list'][0], doc['file_list'][1]
            per_file.setdefault(desc['file_name'], []).extend(x['HashValue'] & 0xFFFFFFFF for x in flags)
        for fname, hashes in per_file.items():
            missing = [h for h in hashes if h not in meta]
            if missing:
                raise ValueError('%s: %d flags have no type in gamedata' % (fname, len(missing)))
            self.files[fname] = sorted(hashes)
        self.flags = meta

    @staticmethod
    def _load_flags(gd):
        meta = {}
        for blob in gd.values():
            for kind, entries in Byml(blob).parse().items():
                base = kind.replace('_array_data', '').replace('_data', '')
                if base not in _WORDS:
                    continue
                for e in entries:
                    iv = e['InitValue']
                    count = len(iv[0]['Values']) if kind.endswith('_array_data') else 1
                    h = e['HashValue'] & 0xFFFFFFFF
                    meta[h] = Flag(e['DataName'], h, kind, _WORDS[base] * count, iv)
        return meta


# ---------------------------------------------------------------------------------------------------------
# Save files
# ---------------------------------------------------------------------------------------------------------

HEADER = 0x0C
FOOTER = b'\xff\xff\xff\xff'


def detect_endian(data, revision_hint=None):
    """'<' or '>' for a flag save file, judged by the revision number in the header (a small number)."""
    if len(data) < HEADER + 4 or data[-4:] != FOOTER:
        raise ValueError('not a BotW flag save file (bad size or footer)')
    le, = struct.unpack_from('<I', data, 0)
    be, = struct.unpack_from('>I', data, 0)
    if le < 0x10000 <= be:
        return '<'
    if be < 0x10000 <= le:
        return '>'
    raise ValueError('cannot tell the byte order (header 0x%08X)' % le)


def read_entries(data):
    """-> (revision, endian, {hash: [raw 4-byte words in file order]}); raises on a malformed file."""
    e = detect_endian(data)
    body = len(data) - HEADER - 4
    if body % 8:
        raise ValueError('size is not a whole number of entries')
    rev, = struct.unpack_from(e + 'I', data, 0)
    runs = {}
    last = None
    for off in range(HEADER, HEADER + body, 8):
        h, = struct.unpack_from(e + 'I', data, off)
        if last is not None and h < last:
            raise ValueError('entries are not sorted by hash')
        last = h
        runs.setdefault(h, []).append(data[off + 4:off + 8])
    return rev, e, runs


def convert_flags(data, fmt, fname, to_endian, report):
    """Rebuild one flag save file for the revision/byte order of `fmt`."""
    src_rev, src_e, runs = read_entries(data)
    out = bytearray(struct.pack(to_endian + 'III', fmt.revision, 0xFFFFFFFF, 1))
    known = set(fmt.files[fname])
    dropped = [h for h in runs if h not in known]
    kept = defaulted = resized = 0
    for h in fmt.files[fname]:
        fl = fmt.flags[h]
        words = runs.get(h)
        if words is None:
            raw = _encode_init(fl.kind, fl.init, fl.words)
            ws = [raw[i:i + 4] for i in range(0, len(raw), 4)]
            # `raw` is little-endian already; strings are bytes and never swapped
            if not fl.is_string and to_endian == '>':
                ws = [w[::-1] for w in ws]
            defaulted += 1
        else:
            if len(words) != fl.words:
                resized += 1
                init = _encode_init(fl.kind, fl.init, fl.words)
                dflt = [init[i:i + 4] for i in range(0, len(init), 4)]
                if not fl.is_string and src_e == '>':
                    dflt = [w[::-1] for w in dflt]
                words = (words + dflt[len(words):])[:fl.words]
            ws = words
            if not fl.is_string and src_e != to_endian:
                ws = [w[::-1] for w in ws]
            kept += 1
        for w in ws:
            out += struct.pack(to_endian + 'I', h) + w
    out += FOOTER
    report.append('  %-14s revision 0x%X -> 0x%X, %d flags kept, %d defaulted, %d dropped, %d resized'
                  % (fname, src_rev, fmt.revision, kept, defaulted, len(dropped), resized))
    if dropped:
        report.append('    dropped (not in the target revision): '
                      + ', '.join(sorted(_describe(h) for h in dropped)[:8]) + (' ...' if len(dropped) > 8 else ''))
    return bytes(out)


def _describe(h):
    return '0x%08X' % h


def convert_tracker(b):
    """One Hero's Path block (a fixed 64 KiB record, swapped in 32-bit words except the 8 byte header: four
    plain bytes, then two 16-bit fields). The swap is its own inverse, so it works in both directions."""
    if len(b) % 4:
        raise ValueError('tracker block size is not a multiple of 4')
    head = b[:4] + b[5:6] + b[4:5] + b[7:8] + b[6:7]
    return head + b''.join(b[i:i + 4][::-1] for i in range(8, len(b), 4))


def crc32_name(name):
    return zlib.crc32(name.encode()) & 0xFFFFFFFF


# ---------------------------------------------------------------------------------------------------------
# Folder level
# ---------------------------------------------------------------------------------------------------------

def _read(path):
    with open(path, 'rb') as f:
        return f.read()


def put(path, data, overwrite, report):
    if os.path.exists(path):
        if not overwrite:
            report.append('  skipped %s (exists; use --overwrite to keep a .bak and replace it)' % path)
            return False
        bak = '%s.bak-%s' % (path, time.strftime('%Y%m%d-%H%M%S'))
        shutil.move(path, bak)
        report.append('  moved existing %s to %s' % (path, os.path.basename(bak)))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    return True


def find_profile_root(src):
    """Accept the profile folder itself or a folder above it (Cemu: .../save/00050000/101c9500/user)."""
    def looks(p):
        return os.path.isdir(p) and any(os.path.isfile(os.path.join(p, s, 'game_data.sav'))
                                        for s in os.listdir(p) if s.isdigit())
    if looks(src):
        return src
    for dirpath, dirs, _ in os.walk(src):
        if looks(dirpath):
            return dirpath
        if dirpath.count(os.sep) - src.count(os.sep) > 6:
            dirs[:] = []
    raise SystemExit('no BotW save slots (numbered folders with game_data.sav) found under %s' % src)


def convert_profile(src, dst, fmt, to_endian, slots, tracker, overwrite, dry_run=False):
    report = []
    root = find_profile_root(src)
    report.append('source: %s' % root)
    report.append('target: revision 0x%X, %s endian' % (fmt.revision, 'little' if to_endian == '<' else 'big'))
    todo = sorted(s for s in os.listdir(root) if s.isdigit() and (slots is None or int(s) in slots))
    results = {}
    src_endian = None

    def emit(rel, data):
        results[rel] = data

    for slot in todo:
        d = os.path.join(root, slot)
        gd = os.path.join(d, 'game_data.sav')
        if not os.path.isfile(gd):
            continue
        report.append('slot %s' % slot)
        if src_endian is None:
            src_endian = detect_endian(_read(gd))
        emit('%s/game_data.sav' % slot, convert_flags(_read(gd), fmt, 'game_data.sav', to_endian, report))
        cap = os.path.join(d, 'caption.sav')
        if os.path.isfile(cap) and 'caption.sav' in fmt.files:
            emit('%s/caption.sav' % slot, convert_flags(_read(cap), fmt, 'caption.sav', to_endian, report))
        jpg = os.path.join(d, 'caption.jpg')
        if os.path.isfile(jpg):
            emit('%s/caption.jpg' % slot, _read(jpg))
    opt = os.path.join(root, 'option.sav')
    if os.path.isfile(opt) and 'option.sav' in fmt.files:
        report.append('options')
        emit('option.sav', convert_flags(_read(opt), fmt, 'option.sav', to_endian, report))
    trk = os.path.join(root, 'tracker')
    if tracker != 'skip' and os.path.isdir(trk):
        # the swap only applies when the byte order changes; a Switch save converted for Switch stays as it is
        swap = tracker == 'convert' and src_endian is not None and src_endian != to_endian
        report.append('tracker (%s)' % ('convert' if swap else 'copy' if tracker == 'convert' else tracker))
        for n in sorted(os.listdir(trk)):
            b = _read(os.path.join(trk, n))
            emit('tracker/' + n, convert_tracker(b) if swap and len(b) % 4 == 0 else b)
    for extra in ('album', 'pict_book'):      # photos and compendium pictures: plain JPEG files
        folder = os.path.join(root, extra)
        if os.path.isdir(folder):
            names = [n for n in sorted(os.listdir(folder)) if n.lower().endswith('.jpg')]
            report.append('%s: %d pictures copied' % (extra, len(names)))
            for n in names:
                emit('%s/%s' % (extra, n), _read(os.path.join(folder, n)))
    if not results:
        raise SystemExit('nothing to convert')
    if not dry_run:
        written = sum(put(os.path.join(dst, *rel.split('/')), data, overwrite, report) for rel, data in results.items())
        report.append('wrote %d of %d files to %s' % (written, len(results), dst))
    return report, results


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', required=True, help='save folder to convert (Cemu: .../user/80000001, or a Switch profile)')
    ap.add_argument('--dst', required=True, help='output folder (Savage Wind: save/save_0)')
    ap.add_argument('--bootup', default=None,
                    help='Pack/Bootup.pack of the TARGET game (default: extracted/romfs/Pack/Bootup.pack)')
    ap.add_argument('--to', choices=('switch', 'wiiu'), default='switch',
                    help='target console, sets the byte order (default: switch, which Savage Wind uses)')
    ap.add_argument('--slots', default=None, help='comma separated slot numbers (default: all)')
    ap.add_argument('--tracker', choices=('convert', 'skip', 'copy'), default='convert')
    ap.add_argument('--overwrite', action='store_true', help='replace existing files (the old ones become .bak)')
    ap.add_argument('--dry-run', action='store_true', help='convert in memory and report, write nothing')
    a = ap.parse_args(argv)
    bootup = a.bootup or os.path.join('extracted', 'romfs', 'Pack', 'Bootup.pack')
    if not os.path.isfile(bootup):
        raise SystemExit('Bootup.pack not found at %s (pass --bootup)' % bootup)
    fmt = Format(bootup)
    slots = {int(x) for x in a.slots.split(',')} if a.slots else None
    report, _ = convert_profile(a.src, a.dst, fmt, '<' if a.to == 'switch' else '>', slots, a.tracker,
                                a.overwrite, a.dry_run)
    print('\n'.join(report))


if __name__ == '__main__':
    main()
