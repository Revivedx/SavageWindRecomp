#!/usr/bin/env python3
"""Compares the save importer in C (runtime/services/save_convert.c) with the Python tool it follows
(tools/botw_save_convert.py): both convert the same save, and every output file has to match byte for byte.

    python3 tools/test_save_convert.py                  synthetic cases (no game files needed) + the C safety tests
    python3 tools/test_save_convert.py --src <Cemu profile folder> --bootup <Pack/Bootup.pack>
                                                        your own save and game files: both implementations convert the
                                                        real save and every file is compared (nothing is written
                                                        outside a temporary folder; the files are never copied anywhere)

The synthetic cases build a small Bootup.pack by hand (SARC, Yaz0 and BYML writers below), so the whole path is
exercised: reading the pack, the flag tables, the conversion, the folder level and the refusals. One PASS / FAIL line
per case; the exit status is 1 when anything failed. Needs a C compiler (cc, clang or gcc; CC=... picks one) unless
--exe gives a test program that is already built (make test-save-convert builds build/test_save_convert).

The last cases run the start screen's Saves tab itself without a window (tools/test_saves_tab.cpp: Dear ImGui builds its
frames in memory, the real importer runs on its worker thread, the question is answered with a click on Import or the Escape key) on copies
of a synthetic save folder. They need a C++ compiler and SDL3's headers (SDL3_INCLUDE=<folder> if they are not on the
default path); without those they are skipped, and --no-ui skips them.
"""
import argparse
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import botw_save_convert as py  # noqa: E402

# ---------------------------------------------------------------------------------------------------------
# Writers for the containers of a pack (the converter itself only reads them)
# ---------------------------------------------------------------------------------------------------------


def byml(doc):
    """Little-endian BYML v2 of a dict / list / str / bool / int / float tree."""
    names, strs = set(), set()

    def collect(v):
        if isinstance(v, dict):
            for k, x in v.items():
                names.add(k)
                collect(x)
        elif isinstance(v, list):
            for x in v:
                collect(x)
        elif isinstance(v, str):
            strs.add(v)
    collect(doc)
    names, strs = sorted(names), sorted(strs)
    out = bytearray(16)

    def pad():
        while len(out) % 4:
            out.append(0)

    def table(items):
        start = len(out)
        offs, body = [], b''
        base = 4 + 4 * (len(items) + 1)
        for s in items:
            offs.append(base + len(body))
            body += s.encode() + b'\0'
        offs.append(base + len(body))
        out.extend(bytes([0xC2]) + len(items).to_bytes(3, 'little') + b''.join(struct.pack('<I', o) for o in offs) + body)
        pad()
        return start
    name_off, str_off = table(names), table(strs)

    def scalar(v):
        if isinstance(v, bool):
            return 0xD0, int(v)
        if isinstance(v, int):
            return (0xD1, v & 0xFFFFFFFF) if -2**31 <= v < 2**31 else (0xD3, v)
        if isinstance(v, float):
            return 0xD2, struct.unpack('<I', struct.pack('<f', v))[0]
        return 0xA0, strs.index(v)

    def node(v):
        if isinstance(v, (dict, list)):
            kids = [node(x) for x in (v.values() if isinstance(v, dict) else v)]  # children first
            pad()
            at = len(out)
            if isinstance(v, list):
                out.extend(bytes([0xC0]) + len(v).to_bytes(3, 'little') + bytes(t for t, _ in kids))
                pad()
                out.extend(b''.join(struct.pack('<I', x) for _, x in kids))
            else:
                out.extend(bytes([0xC1]) + len(v).to_bytes(3, 'little'))
                for k, (t, x) in sorted(zip(v.keys(), kids), key=lambda e: names.index(e[0])):
                    out.extend(names.index(k).to_bytes(3, 'little') + bytes([t]) + struct.pack('<I', x))
            return (0xC0 if isinstance(v, list) else 0xC1), at
        return scalar(v)
    _, root = node(doc)
    out[0:16] = b'YB' + struct.pack('<HIII', 2, name_off, str_off, root)
    return bytes(out)


def sarc(files):
    """Little-endian SARC of {name: bytes}."""
    def h(n):
        r = 0
        for ch in n.encode():
            r = (r * 0x65 + ch) & 0xFFFFFFFF
        return r
    items = sorted(files.items(), key=lambda kv: h(kv[0]))
    names, data, nodes = b'', b'', []
    for n, d in items:
        while len(data) % 4:
            data += b'\0'
        nodes.append((h(n), 0x01000000 | len(names) // 4, len(data), len(data) + len(d)))
        data += d
        names += n.encode() + b'\0'
        while len(names) % 4:
            names += b'\0'
    sfat = b'SFAT' + struct.pack('<HHI', 12, len(items), 0x65) + b''.join(struct.pack('<IIII', *x) for x in nodes)
    sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + names
    data_off = (0x14 + len(sfat) + len(sfnt) + 0x3F) & ~0x3F
    head = b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, data_off + len(data), data_off, 0x100, 0)
    return head + sfat + sfnt + b'\0' * (data_off - 0x14 - len(sfat) - len(sfnt)) + data


def yaz0(d):
    """Yaz0 with back references to the last 16 distances (so the decoder's short and long match forms both occur)."""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(d)) + b'\0' * 8)
    i = 0
    while i < len(d):
        flag, flag_at = 0, len(out)
        out.append(0)
        for bit in range(8):
            if i >= len(d):
                break
            best, dist = 0, 0
            for k in range(1, min(16, i) + 1):
                n = 0
                while n < 0x111 and i + n < len(d) and d[i + n] == d[i + n - k]:
                    n += 1
                if n > best:
                    best, dist = n, k
            if best >= 3:
                dd = dist - 1
                if best >= 0x12:
                    out += bytes([dd >> 8, dd & 0xFF, best - 0x12])
                else:
                    out += bytes([(best - 2) << 4 | dd >> 8, dd & 0xFF])
                i += best
            else:
                flag |= 0x80 >> bit
                out.append(d[i])
                i += 1
        out[flag_at] = flag
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------
# A small game: the flag tables of the old (Wii U) revision the saves come from and the new (Switch) one
# ---------------------------------------------------------------------------------------------------------

NEW_REV, OLD_REV = 0x4730, 0x471B
BULK = 600   # plain s32 flags on top of the named ones: enough that the hash order matters


def specs(old):
    """(name, kind, element count, InitValue) of the game revision; `old` is the Wii U one the saves come from."""
    s = [
        ('Rupees', 's32_data', 1, 0),
        ('Hearts', 's32_data', 1, 12),
        ('Big', 's32_data', 1, 4000000000),
        ('Neg', 's32_data', 1, -5),
        ('Ratio', 'f32_data', 1, 1.5),
        ('SavePos', 'vector3f_data', 1, {'Values': [1.0, 2.0, 3.0]}),
        ('Look', 'vector2f_data', 1, [{'Values': [0.5, -1.5]}]),
        ('Quat', 'vector4f_data', 1, [[1.0, 2.0], [3.0, 4.0]]),
        ('Name', 'string64_data', 1, 'Link'),
        ('Place', 'string256_data', 1, 'Hyrule'),
        ('Tag', 'string_data', 1, 'abc'),
        ('Open', 'bool_data', 1, 1),
        ('OpenF', 'bool_data', 1, 1.0),
        ('Respawn', 'bool_data', 1, 132),
        ('Weather', 's32_array_data', 2 if old else 3, [{'Values': [0] * (2 if old else 3)}]),
        ('Names', 'string64_array_data', 3 if old else 2, [{'Values': ['a', 'bb', 'ccc'][:3 if old else 2]}]),
        ('Spots', 'vector2f_array_data', 2, [{'Values': [[1.0, 2.0], [3.0, 4.0]]}]),
        ('Brightness', 's32_data', 1, 5),
        ('Volume', 'f32_data', 1, 0.75),
        ('PlayTime', 's32_data', 1, 0),
    ]
    s += [('Bulk%04d' % i, 's32_data', 1, i) for i in range(BULK)]
    s += [('OnlyOld1', 's32_data', 1, 0), ('OnlyOld2', 'bool_data', 1, 0)] if old else \
         [('NewA', 's32_data', 1, 7), ('NewB', 'bool_data', 1, 1), ('NewC', 'string64_data', 1, 'x' * 70),
          ('NewD', 'vector3f_data', 1, [[9.5, 8.5, 7.5]]), ('NewE', 'f32_array_data', 2, [{'Values': [0.25, 0.5]}])]
    return s


def hash_value(v):
    return struct.unpack('<i', struct.pack('<I', v))[0] if v & 0x80000000 and v % 2 else v   # a mix of D1 and D3


def files_of(names, old):
    """Which flag file each flag is in (a flag can be in several)."""
    game = [n for n in names if n not in ('Brightness', 'Volume', 'PlayTime', 'OnlyOld2')]
    return {'game_data.sav': game, 'caption.sav': ['Rupees', 'Hearts', 'PlayTime', 'Name'],
            'option.sav': ['Brightness', 'Volume'] + (['OnlyOld2'] if old else [])}


def make_bootup(path, old=False, revision=None, split=2):
    sp = specs(old)
    names = [x[0] for x in sp]
    docs = {}
    for n, kind, count, init in sp:
        e = {'DataName': n, 'DeleteRev': -1, 'HashValue': hash_value(py.crc32_name(n)), 'InitValue': init,
             'IsEventAssociated': True}
        docs.setdefault(kind, []).append(e)
    gd = {}
    for i, (kind, entries) in enumerate(sorted(docs.items())):
        gd['%s_%d.bgdata' % (kind, i)] = byml({kind: entries})
    gd['revival_bool_data_0.bgdata'] = byml({'revival_bool_data': [{'DataName': 'Skipped', 'HashValue': 1, 'InitValue': {}}]})
    fm = {}
    k = 0
    for fname, members in files_of(names, old).items():
        members = [m for m in members if m in names]
        chunk = max(1, len(members) // split)
        for j in range(0, len(members), chunk):
            doc = {'save_info': [{'directory_num': 8, 'revision': revision or (OLD_REV if old else NEW_REV)}],
                   'file_list': [{'file_name': fname, 'addable': False},
                                 [{'HashValue': hash_value(py.crc32_name(m))} for m in members[j:j + chunk]]]}
            fm['saveformat_%d.bgsvdata' % k] = byml(doc)
            k += 1
    pack = sarc({'GameData/gamedata.ssarc': yaz0(sarc(gd)), 'GameData/savedataformat.ssarc': yaz0(sarc(fm)),
                 'Layout/Common.sblarc': b'unrelated', 'Actor/Pack/Other.sbactorpack': b'\0' * 64})
    with open(path, 'wb') as f:
        f.write(pack)


# ---------------------------------------------------------------------------------------------------------
# Synthetic saves
# ---------------------------------------------------------------------------------------------------------

def flag_file(rng, specs_, members, e, revision, skip=(), play=None):
    """A flag save file of the old revision, in byte order e, with random values."""
    by = {py.crc32_name(n): (n, kind, count, init) for n, kind, count, init in specs_}
    out = bytearray(struct.pack(e + 'III', revision, 0xFFFFFFFF, 1))
    for h in sorted(py.crc32_name(m) for m in members):
        n, kind, count, init = by[h]
        if n in skip:
            continue
        base = kind.replace('_array_data', '').replace('_data', '')
        words = py._WORDS[base] * count
        for w in range(words):
            if base.startswith('string'):
                word = bytes(rng.choice(b'abcdefghij\0') for _ in range(4))
            elif n == 'PlayTime' and play is not None:
                word = struct.pack(e + 'i', play)
            elif base == 'bool':
                word = struct.pack(e + 'I', rng.choice((0, 1)))
            elif base == 'f32' or base.startswith('vector'):
                word = struct.pack(e + 'f', rng.uniform(-1000, 1000))
            else:
                word = struct.pack(e + 'I', rng.getrandbits(32))
            out += struct.pack(e + 'I', h) + word
    return bytes(out) + py.FOOTER


def make_profile(path, seed=1, e='>', slots=(0, 1, 2), tracker=True, extras=True, options=True, captions=True):
    rng = random.Random(seed)
    sp = specs(True)
    files = files_of([x[0] for x in sp], True)
    os.makedirs(path, exist_ok=True)

    def w(rel, data):
        full = os.path.join(path, *rel.split('/'))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, 'wb') as f:
            f.write(data)
    for s in slots:
        w('%d/game_data.sav' % s, flag_file(rng, sp, files['game_data.sav'], e, OLD_REV))
        if captions:
            w('%d/caption.sav' % s, flag_file(rng, sp, files['caption.sav'], e, OLD_REV, play=3600 * (s + 1) + 61))
            w('%d/caption.jpg' % s, b'\xff\xd8\xff' + bytes(rng.getrandbits(8) for _ in range(300)) + b'\xff\xd9')
    if options:
        w('option.sav', flag_file(rng, sp, files['option.sav'], e, OLD_REV))
    if tracker:
        for i in range(4):
            w('tracker/trackblock%02d.sav' % i, bytes(rng.getrandbits(8) for _ in range(4096)))
        w('tracker/odd.sav', b'123456789a')       # not a multiple of 4: copied as it is
        w('tracker/empty.sav', b'')
    if extras:
        for i in range(5):
            w('album/img%03d.jpg' % i, bytes(rng.getrandbits(8) for _ in range(200 + i)))
        w('album/UPPER.JPG', b'upper')
        w('album/notes.txt', b'not a picture')
        for i in range(7):
            w('pict_book/%03d.jpg' % i, bytes(rng.getrandbits(8) for _ in range(150 + i)))
    return path


# ---------------------------------------------------------------------------------------------------------
# Running the two implementations and comparing
# ---------------------------------------------------------------------------------------------------------

def tree(path):
    out = {}
    for d, _, files in os.walk(path):
        for n in files:
            full = os.path.join(d, n)
            with open(full, 'rb') as f:
                out[os.path.relpath(full, path).replace(os.sep, '/')] = f.read()
    return out


def run_python(src, bootup, dst):
    """-> (files {rel: bytes} or None, stats {file: tuple}, counts {name: n}, error text)"""
    try:
        fmt = py.Format(bootup)
        report, results = py.convert_profile(src, dst, fmt, '<', None, 'convert', False)
    except (SystemExit, Exception) as ex:  # noqa: BLE001 (the Python tool raises ValueError, SystemExit, ...)
        return None, {}, {}, 'python: %s: %s' % (type(ex).__name__, ex)
    stats, counts, slot = {}, {}, None
    for line in report:
        m = re.match(r'slot (\d+)$', line)
        if m:
            slot = m.group(1)
        elif line == 'options':
            slot = None
        m = re.match(r'  (\S+)\s+revision 0x([0-9A-F]+) -> 0x([0-9A-F]+), (\d+) flags kept, (\d+) defaulted, (\d+) dropped, (\d+) resized$', line)
        if m:
            stats[(slot + '/' if slot is not None else '') + m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16)) + tuple(int(m.group(i)) for i in range(4, 8))
        m = re.match(r'(album|pict_book): (\d+) pictures copied$', line)
        if m:
            counts[m.group(1)] = int(m.group(2))
    return {k: v for k, v in tree(dst).items()}, stats, counts, None


def run_c(exe, src, bootup, dst, extra=()):
    """-> (files or None, stats, counts, error text, exit code)"""
    p = subprocess.run([exe, 'convert', '--src', src, '--bootup', bootup, '--dst', dst, *extra],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    if p.returncode != 0:
        line = [x for x in p.stdout.splitlines() if x.startswith('RESULT')]
        return None, {}, {}, (line[0] if line else 'C: no result: ' + p.stderr[:300]), p.returncode
    stats, counts = {}, {}
    for line in p.stdout.splitlines():
        t = line.split()
        if t[:1] == ['FILE']:
            stats[t[1]] = (int(t[2], 16), int(t[3], 16)) + tuple(int(x) for x in t[4:8])
        elif t[:1] == ['COUNTS']:
            counts = dict(x.split('=') for x in t[1:])
    files = tree(os.path.join(dst, 'save_0'))
    return files, stats, {k: int(v) for k, v in counts.items()}, None, 0


def compare(py_out, c_out):
    """-> list of differences between the two {rel: bytes} trees"""
    diffs = []
    for k in sorted(set(py_out) | set(c_out)):
        if k not in c_out:
            diffs.append('only Python wrote ' + k)
        elif k not in py_out:
            diffs.append('only C wrote ' + k)
        elif py_out[k] != c_out[k]:
            a, b = py_out[k], c_out[k]
            at = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
            diffs.append('%s differs (sizes %d / %d, first difference at byte %d)' % (k, len(a), len(b), at))
    return diffs


class Runner:
    def __init__(self, exe, tmp):
        self.exe, self.tmp, self.failed, self.n = exe, tmp, 0, 0

    def result(self, name, problems, note=''):
        self.n += 1
        if problems:
            self.failed += 1
            print('FAIL  %s' % name)
            for p in problems[:8]:
                print('        ' + p)
            if len(problems) > 8:
                print('        ... %d more' % (len(problems) - 8))
        else:
            note = note.replace(self.tmp, '<tmp>')
            print('PASS  %s%s' % (name, ('  (' + note + ')') if note else ''))

    def both(self, name, src, bootup, c_extra=(), expect_same=True, tag=None):
        """Converts src with both and compares everything; with expect_same False only that both refuse is checked."""
        tag = tag or re.sub(r'\W', '_', name)
        pdst, cdst = os.path.join(self.tmp, tag + '_py'), os.path.join(self.tmp, tag + '_c')
        t0 = time.time()
        pf, ps, pc, perr = run_python(src, bootup, pdst)
        t1 = time.time()
        cf, cs, cc, cerr, code = run_c(self.exe, src, bootup, cdst, c_extra)
        t2 = time.time()
        problems = []
        if pf is None or cf is None:
            problems.append('Python and C disagree on whether it converts: %s | %s' % (perr or 'ok', cerr or 'ok'))
            return self.result(name, problems)
        problems += compare(pf, cf)
        if ps != cs:
            for k in sorted(set(ps) | set(cs)):
                if ps.get(k) != cs.get(k):
                    problems.append('report of %s: Python %s, C %s' % (k, ps.get(k), cs.get(k)))
        for k in ('album', 'pict_book'):
            if pc.get(k, 0) != cc.get('album' if k == 'album' else 'pict', 0):
                problems.append('%s count: Python %s, C %s' % (k, pc.get(k, 0), cc.get('album' if k == 'album' else 'pict', 0)))
        if not pf:
            problems.append('nothing was converted')
        self.result(name, problems, '%d files, Python %.2f s, C %.2f s' % (len(cf), t1 - t0, t2 - t1))

    def refused(self, name, src, bootup, code, c_extra=(), python_too=True, tag=None):
        """C has to refuse with `code` (and leave no output); Python has to refuse as well."""
        tag = tag or re.sub(r'\W', '_', name)
        cdst = os.path.join(self.tmp, tag + '_c')
        cf, _, _, cerr, rc = run_c(self.exe, src, bootup, cdst, c_extra)
        problems = []
        if cf is not None or rc != 1:
            problems.append('C converted (or crashed: exit %d) instead of refusing' % rc)
        elif not cerr.startswith('RESULT error %d ' % code):
            problems.append('wrong reason: %s (wanted code %d)' % (cerr, code))
        if os.path.exists(cdst):
            problems.append('C left %s behind' % cdst)
        if python_too:
            pf, _, _, perr = run_python(src, bootup, os.path.join(self.tmp, tag + '_py'))
            if pf is not None:
                problems.append('Python converted what C refuses')
        self.result(name, problems, cerr or '')


# the codes of ScCode (save_convert.h)
NO_SAVE, SWITCH_SAVE, BAD_SAVE, NO_BOOTUP, BAD_BOOTUP, IO = 1, 2, 3, 4, 5, 6


def build_exe(tmp, cc=None):
    exe = os.path.join(tmp, 'test_save_convert' + ('.exe' if os.name == 'nt' else ''))
    for c in ([cc] if cc else [os.environ.get('CC'), 'cc', 'clang', 'gcc']):
        if not c or not shutil.which(c):
            continue
        cmd = [c, '-std=gnu11', '-O1', '-D_FILE_OFFSET_BITS=64', '-I' + os.path.join(ROOT, 'runtime'), '-o', exe,
               os.path.join(HERE, 'test_save_convert.c'), os.path.join(ROOT, 'runtime', 'services', 'archive.c')]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode == 0:
            return exe
        print('compiling with %s failed:\n%s' % (c, p.stderr[-2000:]))
    sys.exit('no C compiler found (set CC, or pass --exe)')


def synthetic(r):
    tmp = r.tmp
    boot = os.path.join(tmp, 'Bootup.pack')
    make_bootup(boot)

    prof = make_profile(os.path.join(tmp, 'cemu', 'mlc01', 'usr', 'save', '00050000', '101c9500', 'user', '80000001'), seed=1)
    r.both('Wii U save, 3 slots, options, Hero\'s Path, album, compendium', prof, boot)
    p = subprocess.run([r.exe, 'oom', prof, boot, os.path.join(tmp, 'oom_stage')], capture_output=True, text=True)
    r.result('each reallocation can fail without a crash or a leftover stage',
             [] if p.returncode == 0 and p.stdout.startswith('PASS ') else [p.stdout + p.stderr])
    p = subprocess.run([r.exe, 'cancel', prof, boot, os.path.join(tmp, 'cancel_stage')], capture_output=True, text=True)
    r.result('cancellation during search and between files discards the stage',
             [] if p.returncode == 0 and p.stdout.startswith('PASS ') else [p.stdout + p.stderr])
    r.both('picked folder above the profile (with other folders beside it)', os.path.join(tmp, 'cemu'), boot)
    os.makedirs(os.path.join(tmp, 'cemu', 'mlc01', 'usr', 'save', '00050000', '101c9500', 'user', 'common'), exist_ok=True)

    # the Saves tab dates a slot by the file time of its caption.sav: converted and copied files keep their source's
    dated = make_profile(os.path.join(tmp, 'dated', 'user', '80000001'), seed=8)
    stamps = {'0/caption.sav': 1500000000, '1/game_data.sav': 1500000100, '0/caption.jpg': 1500000200,
              'tracker/trackblock00.sav': 1500000300, 'album/img000.jpg': 1500000400, 'option.sav': 1500000500}
    for rel, t in stamps.items():
        os.utime(os.path.join(dated, *rel.split('/')), (t, t))
    cf, _, _, cerr, _ = run_c(r.exe, dated, boot, os.path.join(tmp, 'dated_c'))
    problems = [cerr] if cf is None else []
    for rel, t in stamps.items():
        out = os.path.join(tmp, 'dated_c', 'save_0', *rel.split('/'))
        if cf is not None and int(os.stat(out).st_mtime) != t:
            problems.append('%s: file time %d, wanted %d' % (rel, int(os.stat(out).st_mtime), t))
    r.result('converted and copied files keep the file time of their source', problems)

    only = make_profile(os.path.join(tmp, 'only_game_data'), seed=2, slots=(0,), tracker=False, extras=False, options=False,
                        captions=False)
    r.both('a single slot with nothing but game_data.sav', only, boot)
    r.both('ten slots (names sort as text)', make_profile(os.path.join(tmp, 'many'), seed=3, slots=tuple(range(11)),
                                                          tracker=False, extras=False), boot)

    if sys.platform == 'win32':
        # the game's .exe reads UTF-8 paths (runtime/platform/windows.manifest); this test program has no such manifest
        print('SKIP  folders with non-ASCII names (this test program has no UTF-8 manifest like the game\'s .exe)')
    else:
        uni = os.path.join(tmp, 'Spielstände 日本語 ñ')
        make_profile(os.path.join(uni, 'user', '80000001'), seed=1)
        r.both('folders with non-ASCII names', uni, boot, tag='unicode')

    # the game's own revision differs: more flags in the target, fewer in the old one (already covered); another revision number
    boot2 = os.path.join(tmp, 'Bootup2.pack')
    make_bootup(boot2, revision=0x471E)
    r.both('another game revision (0x471E)', prof, boot2)

    # a Switch save: refused, or converted with the test flag, tracker copied as it is
    sw = make_profile(os.path.join(tmp, 'switch', 'user', '80000001'), seed=4, e='<')
    r.refused('a Switch save is refused and nothing is written', sw, boot, SWITCH_SAVE, python_too=False)
    r.both('a Switch save with the test flag: same as Python, Hero\'s Path unchanged', sw, boot, c_extra=('--allow-switch',),
           tag='switch_allowed')

    # refusals: nothing may be left behind
    empty = os.path.join(tmp, 'empty')
    os.makedirs(os.path.join(empty, 'a', 'b'))
    r.refused('a folder with no save in it', empty, boot, NO_SAVE)
    r.refused('a folder that is not there', os.path.join(tmp, 'nowhere'), boot, NO_SAVE)
    r.refused('Bootup.pack missing', prof, os.path.join(tmp, 'no_such.pack'), NO_BOOTUP, python_too=False)
    junk = os.path.join(tmp, 'junk.pack')
    with open(junk, 'wb') as f:
        f.write(os.urandom(5000))
    r.refused('Bootup.pack that is not a pack', prof, junk, BAD_BOOTUP, python_too=False)
    with open(boot, 'rb') as f:
        half = f.read()
    cut = os.path.join(tmp, 'cut.pack')
    with open(cut, 'wb') as f:
        f.write(half[:len(half) // 2])
    r.refused('Bootup.pack cut in half', prof, cut, BAD_BOOTUP, python_too=False)

    def damaged(name, how, code=BAD_SAVE):
        p = make_profile(os.path.join(tmp, 'dmg_' + re.sub(r'\W', '_', name), 'user', '80000001'), seed=5)
        target = os.path.join(p, '2', 'game_data.sav')   # slots 0 and 1 convert fine before this one fails
        with open(target, 'rb') as f:
            d = f.read()
        with open(target, 'wb') as f:
            f.write(how(d))
        r.refused(name, p, boot, code)
    damaged('a save copied only halfway (no footer)', lambda d: d[:len(d) // 2])
    damaged('a save cut inside an entry', lambda d: d[:-6] + b'\xff\xff\xff\xff')
    damaged('a save that is not a flag file', lambda d: b'\0' * 64)
    damaged('a save with entries out of order', lambda d: d[:12] + d[20:28] + d[12:20] + d[28:])
    p = make_profile(os.path.join(tmp, 'dmg_caption', 'user', '80000001'), seed=6)
    with open(os.path.join(p, '1', 'caption.sav'), 'wb') as f:
        f.write(b'broken')
    r.refused('a damaged caption.sav', p, boot, BAD_SAVE)

    # slots as the tab lists them
    s0 = os.path.join(tmp, 'listing')
    make_profile(s0, seed=7, slots=(1, 0, 10, 2), e='<', tracker=False, extras=False)
    os.makedirs(os.path.join(s0, 'tracker'))
    os.utime(os.path.join(s0, '10', 'caption.sav'), (1700000000, 1700000000))
    os.remove(os.path.join(s0, '2', 'caption.sav'))
    os.remove(os.path.join(s0, '2', 'caption.jpg'))
    os.utime(os.path.join(s0, '2', 'game_data.sav'), (1600000000, 1600000000))
    p = subprocess.run([r.exe, 'slots', s0], capture_output=True, text=True)
    rows = [l.split() for l in p.stdout.splitlines()]
    want = [['SLOT', '0', None, '3661', '1'], ['SLOT', '1', None, '7261', '1'], ['SLOT', '2', '1600000000', '-1', '0'],
            ['SLOT', '10', '1700000000', '39661', '1']]
    problems = []
    if [x[:2] for x in rows] != [x[:2] for x in want]:
        problems.append('slots listed: %s' % [x[1] for x in rows])
    for got, w in zip(rows, want):
        if (w[2] is not None and got[2] != w[2]) or got[3:] != w[3:]:
            problems.append('slot %s: %s, wanted %s' % (w[1], got[2:], w[2:]))
    r.result('slot list: numeric order, saved time, play time from caption.sav, picture', problems)

    # the C safety tests: backup, install, rollback, discard
    scratch = os.path.join(tmp, 'selftest')
    p = subprocess.run([r.exe, 'selftest', scratch], capture_output=True, text=True)
    r.result('backup, install, rollback, discard and stale cleanup keep the player\'s saves', [] if 'SELFTEST PASS' in p.stdout else
             (p.stdout + p.stderr).splitlines()[-8:])

    # a second process as the running game: it holds the save folder's lock, the importer sees it, and the lock goes away
    # with the process however that ends (a crashed game leaves nothing stale)
    held = os.path.join(tmp, 'lock_held', 'save')
    os.makedirs(os.path.dirname(held))

    def lock_state():
        return subprocess.run([r.exe, 'locked', held], capture_output=True, text=True).stdout.strip()
    problems = []
    if lock_state() != 'FREE':
        problems.append('a lock nobody holds is not free: ' + lock_state())
    game = subprocess.Popen([r.exe, 'holdlock', held], stdout=subprocess.PIPE, text=True)
    if game.stdout.readline().strip() != 'READY':
        problems.append('the holder did not get the lock')
    elif lock_state() != 'BUSY':
        problems.append('a lock held by another process is not seen as held: ' + lock_state())
    game.kill()   # (not a clean exit: the game crashed)
    game.wait()
    if lock_state() != 'FREE':
        problems.append('the lock outlived its process: ' + lock_state())
    r.result('the game\'s lock on the save folder is seen by another process and ends with it', problems)


def build_ui(tmp):
    """Builds tools/test_saves_tab.cpp -> path, or (None, why)."""
    cxx = next((c for c in (os.environ.get('CXX'), 'c++', 'clang++', 'g++') if c and shutil.which(c)), None)
    cc = next((c for c in (os.environ.get('CC'), 'cc', 'clang', 'gcc') if c and shutil.which(c)), None)
    if not cxx or not cc:
        return None, 'no C++ compiler'
    sdl = []
    if os.environ.get('SDL3_INCLUDE'):
        sdl = ['-isystem', os.environ['SDL3_INCLUDE']]
    elif shutil.which('pkg-config'):
        p = subprocess.run(['pkg-config', '--cflags', 'sdl3'], capture_output=True, text=True)
        sdl = p.stdout.split() if p.returncode == 0 else []
    im, rt, st = (os.path.join(ROOT, 'third_party', 'imgui'), os.path.join(ROOT, 'runtime'),
                  os.path.join(ROOT, 'third_party', 'stb'))
    d = os.path.join(tmp, 'ui')
    os.makedirs(d)
    cxxflags = ['-std=c++17', '-O0', '-D_FILE_OFFSET_BITS=64', '-I' + im, '-I' + rt, '-I' + os.path.join(rt, 'ui'), '-I' + st] + sdl
    jobs = [(cxx, ['-x', 'c++'] + cxxflags, os.path.join(im, n + '.cpp')) for n in ('imgui', 'imgui_draw', 'imgui_tables', 'imgui_widgets')]
    jobs += [(cxx, ['-x', 'c++'] + cxxflags, os.path.join(rt, 'ui', 'option_rows.mm')),
             (cxx, cxxflags, os.path.join(HERE, 'test_saves_tab.cpp'))]
    cflags = ['-std=gnu11', '-O1', '-D_FILE_OFFSET_BITS=64', '-I' + rt, '-I' + st]
    jobs += [(cc, cflags, os.path.join(rt, 'imageload.c')), (cc, cflags + ['-DSC_TEST'], os.path.join(rt, 'services', 'save_convert.c')),
             (cc, cflags, os.path.join(rt, 'services', 'archive.c'))]
    procs, objs = [], []
    for i, (c, flags, src) in enumerate(jobs):
        o = os.path.join(d, '%d.o' % i)
        objs.append(o)
        procs.append(subprocess.Popen([c, '-c'] + flags + ['-o', o, src], stderr=subprocess.PIPE, text=True))
    errs = [p.communicate()[1] for p in procs]
    bad = [e for p, e in zip(procs, errs) if p.returncode]
    if bad:
        return None, ('SDL3 headers not found (set SDL3_INCLUDE)' if 'SDL3/SDL.h' in bad[0] else bad[0][-600:])
    exe = os.path.join(d, 'test_saves_tab' + ('.exe' if os.name == 'nt' else ''))
    p = subprocess.run([cxx, '-o', exe] + objs + ['-lpthread'], capture_output=True, text=True)
    return (exe, None) if p.returncode == 0 else (None, p.stderr[-600:])


def ui_run(exe, scenario, save, game, src=None, cwd=None):
    cmd = [exe, scenario, '--save', save, '--game', game] + (['--src', src] if src else [])
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=180, cwd=cwd)
    out = {}
    for line in p.stdout.splitlines():
        k, _, v = line.partition(' ')
        out[k] = v
    return p.returncode, out, p.stdout + p.stderr[-500:]


def ui_cases(r):
    exe, why = build_ui(r.tmp)
    if not exe:
        print('SKIP  the Saves tab on its own (%s)' % why)
        return
    base = os.path.join(r.tmp, 'ui_world')
    game = os.path.join(base, 'game')
    os.makedirs(os.path.join(game, 'romfs', 'Pack'))
    make_bootup(os.path.join(game, 'romfs', 'Pack', 'Bootup.pack'))
    cemu = make_profile(os.path.join(base, 'cemu', 'user', '80000001'), seed=11)
    py_out = run_python(cemu, os.path.join(game, 'romfs', 'Pack', 'Bootup.pack'), os.path.join(base, 'py_expected'))[0]

    picture = os.path.join(r.tmp, 'picture.jpg')   # a picture the tab can show (the synthetic saves hold random bytes)
    subprocess.run([exe, 'jpeg', picture], check=True)
    with open(picture, 'rb') as f:
        picture = f.read()

    def fresh(name, saves=True):
        """A data folder with the player's Switch save in data/save (and an SD card folder), and its copy."""
        data = os.path.join(r.tmp, 'ui_' + name)
        save = os.path.join(data, 'save')
        os.makedirs(data)
        if saves:
            make_profile(os.path.join(save, 'save_0'), seed=9, e='<', slots=(0, 1), tracker=True, extras=True)
            for slot in (0, 1):
                with open(os.path.join(save, 'save_0', str(slot), 'caption.jpg'), 'wb') as f:
                    f.write(picture)
            os.makedirs(os.path.join(save, 'sdcard'))
            with open(os.path.join(save, 'sdcard', 'note.txt'), 'w') as f:
                f.write('SD')
        return data, save, (tree(save) if saves else {})

    def leftovers(data):
        return sorted(n for n in os.listdir(data) if n.startswith('save-import-'))

    # import over an existing save, the question answered with a click on Import
    data, save, before = fresh('import')
    rc, o, log = ui_run(exe, 'import', save, game, os.path.dirname(os.path.dirname(cemu)), cwd=data)
    problems = []
    if rc != 0 or o.get('DONE') is None:
        problems.append('the harness failed (exit %d): %s' % (rc, log[-400:]))
    else:
        if o.get('KIND') != '1':
            problems.append('not imported: %s' % o)
        problems += ['new save: ' + x for x in compare(py_out, {k[len('save_0/'):]: v for k, v in tree(save).items() if k.startswith('save_0/')})]
        backups = os.path.join(data, 'save-backups')
        names = os.listdir(backups) if os.path.isdir(backups) else []
        if len(names) != 1:
            problems.append('wanted one backup folder, found %s' % names)
        else:
            if tree(os.path.join(backups, names[0])) != before:
                problems.append('the backup is not the old save folder')
            if os.path.join(backups, names[0]) != o.get('BACKUP'):
                problems.append('the popup named another folder: %s' % o.get('BACKUP'))
        if tree(save).get('sdcard/note.txt') != b'SD':
            problems.append('the SD card folder was not carried over')
        if leftovers(data):
            problems.append('staging folder left behind: %s' % leftovers(data))
        if o.get('TEXTURES') != 'created=2 freed=2':
            problems.append('pictures: %s' % o.get('TEXTURES'))
        if o.get('PICTURES') != '2' or o.get('SLOTS') != '2' or o.get('SLOTS_AFTER') != '3':
            problems.append('slot list: %s' % {k: o.get(k) for k in ('SLOTS', 'PICTURES', 'SLOTS_AFTER')})
    r.result('Saves tab: import over an existing save (popup, backup, swap, list)', problems)

    # Escape on the popup: everything as it was
    data, save, before = fresh('cancel')
    rc, o, log = ui_run(exe, 'cancel', save, game, os.path.dirname(os.path.dirname(cemu)), cwd=data)
    problems = []
    if rc != 0 or o.get('KIND') != '5':
        problems.append('not cancelled: %s' % log[-300:])
    if tree(save) != before:
        problems.append('the save folder changed')
    if leftovers(data) or os.path.exists(os.path.join(data, 'save-backups')):
        problems.append('left behind: %s' % os.listdir(data))
    r.result('Saves tab: Escape on the confirmation changes nothing and removes the staged copy', problems)

    # no save yet: nothing to back up, the import just makes the folder
    data, save, _ = fresh('first', saves=False)
    rc, o, log = ui_run(exe, 'import', save, game, os.path.dirname(os.path.dirname(cemu)), cwd=data)
    problems = []
    if rc != 0 or o.get('KIND') != '1':
        problems.append('not imported: %s' % log[-300:])
    else:
        problems += compare(py_out, {k[len('save_0/'):]: v for k, v in tree(save).items() if k.startswith('save_0/')})
        if os.path.exists(os.path.join(data, 'save-backups')) or leftovers(data):
            problems.append('left behind: %s' % os.listdir(data))
    r.result('Saves tab: first import, with no save folder yet', problems)

    # refusals picked in the tab: the message, and the save folder exactly as it was
    sw = make_profile(os.path.join(base, 'switch_pick', 'user', '80000001'), seed=4, e='<')
    empty = os.path.join(base, 'empty_pick')
    os.makedirs(empty)
    bad = make_profile(os.path.join(base, 'bad_pick', 'user', '80000001'), seed=5)
    with open(os.path.join(bad, '1', 'game_data.sav'), 'r+b') as f:
        f.truncate(100)
    for name, folder, code, g in (('a Switch save', os.path.dirname(os.path.dirname(sw)), '2', game),
                                  ('a folder with no save', empty, '1', game),
                                  ('a damaged save', os.path.dirname(os.path.dirname(bad)), '3', game),
                                  ('game files without Bootup.pack', os.path.dirname(os.path.dirname(cemu)), '4', os.path.join(base, 'nogame'))):
        data, save, before = fresh('refuse_' + re.sub(r'\W', '_', name))
        rc, o, log = ui_run(exe, 'pick', save, g, folder, cwd=data)
        problems = []
        if rc != 0 or o.get('KIND') != '2' or o.get('CODE') != code:
            problems.append('wanted a refusal with code %s: %s' % (code, {k: o.get(k) for k in ('KIND', 'CODE', 'DETAIL')}))
        if tree(save) != before:
            problems.append('the save folder changed')
        if leftovers(data) or os.path.exists(os.path.join(data, 'save-backups')):
            problems.append('left behind: %s' % os.listdir(data))
        r.result('Saves tab: %s is refused and nothing changes' % name, problems)

    # another Savage Wind is in game (its lock is held): import and backup refuse, with the message, and change nothing
    for scenario, kind in (('locked-import', '2'), ('locked-backup', '4')):
        data, save, before = fresh(scenario.replace('-', '_'))
        rc, o, log = ui_run(exe, scenario, save, game, cemu, cwd=data)
        problems = []
        if rc != 0 or o.get('KIND') != kind or o.get('CODE') != '11':
            problems.append('wanted a refusal with code 11 (in use): %s' % {k: o.get(k) for k in ('KIND', 'CODE', 'DETAIL')} or log[-300:])
        if tree(save) != before:
            problems.append('the save folder changed')
        if leftovers(data) or os.path.exists(os.path.join(data, 'save-backups')):
            problems.append('left behind: %s' % [x for x in os.listdir(data) if x != 'save'])
        r.result('Saves tab: %s while a game is running is refused and nothing changes' % scenario.split('-')[1], problems)

    for scenario in ('quit-convert', 'quit-confirm', 'quit-install'):
        data, save, before = fresh(scenario)
        rc, o, log = ui_run(exe, scenario, save, game, cemu, cwd=data)
        problems = []
        if rc != 0:
            problems.append(log[-600:])
        if leftovers(data):
            problems.append('staged folder left behind: %s' % leftovers(data))
        if scenario == 'quit-install':
            backups = os.path.join(data, 'save-backups')
            names = os.listdir(backups) if os.path.isdir(backups) else []
            if len(names) != 1 or tree(os.path.join(backups, names[0])) != before:
                problems.append('the previous saves were not backed up intact')
            problems += compare(py_out, {k[len('save_0/'):]: v for k, v in tree(save).items() if k.startswith('save_0/')})
        elif tree(save) != before:
            problems.append('the original save folder changed')
        r.result('Saves tab: %s joins the worker and leaves intact saves' % scenario, problems)

    data, save, before = fresh('recovery', saves=False)
    stage = os.path.join(data, 'save-import-20261030-123456')
    backup = os.path.join(data, 'save-backups', 'save-20261030-123456')
    make_profile(os.path.join(stage, 'save_0'), seed=21)
    make_profile(os.path.join(backup, 'save_0'), seed=22)
    old_stage, old_backup = tree(stage), tree(backup)
    rc, o, log = ui_run(exe, 'recovery', save, game, cwd=data)
    r.result('Saves tab: interrupted installation keeps both copies and names the recovery folders',
             [] if (rc == 0 and o.get('RECOVERY_IMPORT') == stage and o.get('RECOVERY_BACKUP') == backup
                    and not os.path.exists(save) and tree(stage) == old_stage and tree(backup) == old_backup) else [log[-600:]])

    # the Back up saves button
    data, save, before = fresh('backup')
    rc, o, log = ui_run(exe, 'backup', save, game, cwd=data)
    problems = []
    backups = os.path.join(data, 'save-backups')
    names = os.listdir(backups) if os.path.isdir(backups) else []
    if rc != 0 or o.get('KIND') != '3' or len(names) != 1:
        problems.append('no backup: %s' % log[-300:])
    else:
        if tree(os.path.join(backups, names[0])) != before or tree(save) != before:
            problems.append('the copy or the save folder differs')
        if o.get('FILES') != str(len(before)):
            problems.append('files counted: %s, copied: %d' % (o.get('FILES'), len(before)))
    r.result('Saves tab: Back up saves copies the save folder and leaves it where it is', problems)
    data, save, before = fresh('backup_none', saves=False)
    rc, o, log = ui_run(exe, 'backup', save, game, cwd=data)
    r.result('Saves tab: Back up saves with no saves says so and makes nothing',
             [] if (rc == 0 and o.get('KIND') == '4' and o.get('CODE') == '1' and not os.path.exists(os.path.join(data, 'save-backups'))) else [log[-300:]])


def real(r, src, bootup, allow_switch):
    if not os.path.isfile(bootup):
        sys.exit('Bootup.pack not found: %s' % bootup)
    r.both('your save: %s' % src, src, bootup, c_extra=('--allow-switch',) if allow_switch else (), tag='real')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', help='your Wii U / Cemu profile folder (or a folder above it): compare both on the real save')
    ap.add_argument('--bootup', help='your Pack/Bootup.pack (with --src)')
    ap.add_argument('--exe', help='a built test_save_convert (default: built from the sources into a temporary folder)')
    ap.add_argument('--cc', help='C compiler to build it with')
    ap.add_argument('--allow-switch', action='store_true', help='with --src: the save is a Switch one')
    ap.add_argument('--no-ui', action='store_true', help='skip the cases that run the Saves tab (they need a C++ compiler and SDL3 headers)')
    ap.add_argument('--keep', action='store_true', help='keep the temporary folder (printed at the end)')
    a = ap.parse_args()
    if bool(a.src) != bool(a.bootup):
        ap.error('--src and --bootup go together')
    tmp = tempfile.mkdtemp(prefix='swr_save_convert_')
    try:
        exe = a.exe or build_exe(tmp, a.cc)
        r = Runner(exe, tmp)
        if a.src:
            real(r, a.src, a.bootup, a.allow_switch)
        else:
            synthetic(r)
            if not a.no_ui:
                ui_cases(r)
        print('%d of %d cases passed' % (r.n - r.failed, r.n))
        return 1 if r.failed else 0
    finally:
        if a.keep:
            print('kept: ' + tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
