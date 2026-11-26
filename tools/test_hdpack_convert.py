#!/usr/bin/env python3
"""Tests tools/hdpack_convert.py on a synthetic game and pack (no game files needed): small SARC archives, Yaz0 files
and a ResourceSizeTable made here, laid out like a 1.9.0 romfs/ + aoc1/ and a 1.6.0 texture pack.

    python3 tools/test_hdpack_convert.py

One PASS / FAIL line per check; the exit status is 1 when anything failed.
"""
import os
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdpack_convert as hc  # noqa: E402

failed = 0


def check(name, ok):
    global failed
    print(('PASS ' if ok else 'FAIL ') + name)
    failed += not ok


def sarc(files, align=0x80):
    """A little-endian SARC of {name: bytes}, names sorted by hash as the game's are, each file aligned."""
    def h(n):
        v = 0
        for c in n.encode():
            v = (v * 0x65 + c) & 0xFFFFFFFF
        return v
    items = sorted(files.items(), key=lambda kv: h(kv[0]))
    names = b''
    offs = []
    for n, _ in items:
        offs.append(len(names) // 4)
        names += n.encode() + b'\0'
        names += bytes(-len(names) % 4)
    nodes = b''
    data = b''
    for (n, d), o in zip(items, offs):
        data += bytes(-len(data) % align)
        nodes += struct.pack('<IIII', h(n), 0x01000000 | o, len(data), len(data) + len(d))
        data += d
    sfat = b'SFAT' + struct.pack('<HHI', 12, len(items), 0x65) + nodes
    sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + names
    head_len = 0x14 + len(sfat) + len(sfnt)
    data_off = head_len + (-head_len % align)
    head = b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, data_off + len(data), data_off, 0x100, 0)
    return head + sfat + sfnt + bytes(data_off - head_len) + data


def rstb(crc, names):
    out = b'RSTB' + struct.pack('<II', len(crc), len(names))
    for c in sorted(crc):
        out += struct.pack('<II', c, crc[c])
    for n, v in names.items():
        out += n.encode().ljust(128, b'\0') + struct.pack('<I', v)
    return hc.yaz0_store(out)


def put(root, rel, data):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(data)


def tex(n, size):
    return hc.yaz0_store(bytes([n]) * size)  # (a texture file: Yaz0, decompressed size `size`)


def crc(name):
    return zlib.crc32(name.encode())


def make(root):
    game, pack = os.path.join(root, 'game'), os.path.join(root, 'pack')
    g, a = os.path.join(game, 'romfs'), os.path.join(game, 'aoc1')
    # the game (1.9.0)
    put(g, 'Model/Rock.Tex1.sbfres', tex(1, 1000))
    put(g, 'Model/Terrain.Tex1.sbfres', tex(2, 1000))
    put(g, 'Terrain/A/MainField.tscb', b'tscb' * 10)
    put(g, 'Pack/Dungeon000.pack', sarc({'Model/DgnMrgPrt_Dungeon000.Tex1.sbfres': tex(3, 500),
                                         'Actor/ActorLink/Dungeon000.bxml': b'1.9.0 actor'}))
    put(g, 'Pack/Bootup.pack', sarc({'GameData/gamedata.ssarc': b'1.9.0 flags'}))
    put(g, 'Font/Font_US.sbfarc', hc.yaz0_store(sarc({'Normal_00.bffnt': b'n' * 64, 'External_00.bffnt': b'e' * 64})))
    put(a, 'Pack/RemainsElectric.pack', sarc({'Model/Remains.Tex1.sbfres': tex(4, 800)}))
    put(g, hc.RSTB_PATH, rstb({crc('Model/Rock.Tex1.bfres'): 2000, crc('Model/Terrain.Tex1.bfres'): 2000,
                               crc('Terrain/A/MainField.tscb'): 100, crc('Model/DgnMrgPrt_Dungeon000.Tex1.bfres'): 900,
                               crc('Font/Font_US.bfarc'): 500, crc('Font/Normal_00.bffnt'): 100},
                              {'Pack/Dungeon000.pack': 2000, 'Aoc/0010/Pack/RemainsElectric.pack': 2000}))
    # the pack (1.6.0): bigger textures, a changed non-texture file, files 1.9.0 hasn't got, its own table
    put(pack, 'Model/Rock.Tex1.sbfres', tex(11, 9000))
    put(pack, 'Model/Terrain.Tex1.sbfres', tex(12, 9000))
    put(pack, 'Model/OnlyIn160.Tex1.sbfres', tex(13, 100))
    put(pack, 'Terrain/A/MainField.tscb', b'TSCB' * 10)
    put(pack, 'Pack/Dungeon000.pack', sarc({'Model/DgnMrgPrt_Dungeon000.Tex1.sbfres': tex(14, 4000),
                                            'Actor/ActorLink/Dungeon000.bxml': b'1.6.0 actor'}))
    put(pack, 'Pack/Bootup.pack', sarc({'GameData/gamedata.ssarc': b'1.6.0 flags'}))
    put(pack, 'Pack/RemainsElectric.pack', sarc({'Model/Remains.Tex1.sbfres': tex(15, 6400)}))
    put(pack, 'Font/Font_US.sbfarc', hc.yaz0_store(sarc({'Normal_00.bffnt': b'N' * 256, 'External_00.bffnt': b'E' * 256})))
    put(pack, hc.RSTB_PATH, rstb({crc('Model/Rock.Tex1.bfres'): 50000}, {}))
    return game, pack


def tree(root):
    return sorted(os.path.relpath(os.path.join(d, f), root).replace(os.sep, '/') for d, _, fs in os.walk(root) for f in fs)


def main():
    # the containers
    for d in (b'', b'abc', bytes(range(256)) * 9):
        check(f'Yaz0 store / decompress round trip ({len(d)} bytes)', hc.yaz0_decompress(hc.yaz0_store(d)) == d)
    packed = b'Yaz0' + struct.pack('>I', 14) + bytes(8) + bytes([0b11000000]) + b'ab' + bytes([0xA0, 0x01])
    check('Yaz0 back-references (overlapping run)', hc.yaz0_decompress(packed) == b'ab' * 7)
    arc = sarc({'a.bin': b'x' * 3, 'b.bin': b'y' * 5})
    rep = hc.sarc_replace(arc, {'a.bin': b'z' * 300})
    check('SARC replace keeps the other files', hc.sarc_files(rep) == {'a.bin': b'z' * 300, 'b.bin': b'y' * 5})
    off, _, ents = hc._sarc_entries(rep)
    check('SARC replace keeps each file\'s alignment', all((off + s) % 0x80 == 0 for _, _, s, _ in ents))
    check('ResourceSizeTable names', (hc.rstb_name('Model/X.Tex1.sbfres'), hc.rstb_name('Pack/X.pack'),
                                      hc.rstb_name('Foo/x.sarc'), hc.rstb_name('aoc/0010/Pack/X.pack')) ==
          ('Model/X.Tex1.bfres', 'Pack/X.pack', 'Foo/x.sarc', 'Aoc/0010/Pack/X.pack'))

    with tempfile.TemporaryDirectory() as t:
        game, pack = make(t)
        mod = os.path.join(t, 'mods', 'HD')
        os.makedirs(os.path.join(mod, 'romfs', 'stale'))
        before = tree(t)
        hc.convert(game, pack, mod, log=lambda s: None)
        out = os.path.join(mod, 'romfs')
        files = tree(out)
        check('only the mod\'s romfs/ is written', [p for p in tree(t) if p not in before and
                                                    not p.startswith('mods/HD/romfs/')] == [])
        check('an old romfs/ in the mod is replaced', not any(p.startswith('stale') for p in files))
        check('the mod\'s files', files == sorted([
            'Font/Font_US.sbfarc', 'Model/Rock.Tex1.sbfres', 'Model/Terrain.Tex1.sbfres', 'Pack/Dungeon000.pack',
            'Terrain/A/MainField.tscb', hc.RSTB_PATH, 'aoc/0010/Pack/RemainsElectric.pack']))
        check('Bootup.pack and files 1.9.0 hasn\'t got are left out',
              'Pack/Bootup.pack' not in files and 'Model/OnlyIn160.Tex1.sbfres' not in files)
        d = hc.sarc_files(open(os.path.join(out, 'Pack/Dungeon000.pack'), 'rb').read())
        check('a pack: the pack\'s texture, 1.9.0\'s other files',
              d['Model/DgnMrgPrt_Dungeon000.Tex1.sbfres'] == tex(14, 4000) and
              d['Actor/ActorLink/Dungeon000.bxml'] == b'1.9.0 actor')
        d = hc.sarc_files(open(os.path.join(out, 'aoc/0010/Pack/RemainsElectric.pack'), 'rb').read())
        check('a DLC pack goes under aoc/0010/', d['Model/Remains.Tex1.sbfres'] == tex(15, 6400))
        f = hc.sarc_files(hc.yaz0_decompress(open(os.path.join(out, 'Font/Font_US.sbfarc'), 'rb').read()))
        check('fonts: the pack\'s, External_00.bffnt kept', f == {'Normal_00.bffnt': b'N' * 256,
                                                                    'External_00.bffnt': b'e' * 64})
        c, n = hc.rstb_read(open(os.path.join(out, hc.RSTB_PATH), 'rb').read())
        check('table: the pack\'s own size where it is bigger', c[crc('Model/Rock.Tex1.bfres')] == 50000)
        check('table: 1.9.0 size + growth + margin', c[crc('Model/Terrain.Tex1.bfres')] == 2000 + 8000 + hc.RSTB_MARGIN)
        check('table: a texture inside a pack', c[crc('Model/DgnMrgPrt_Dungeon000.Tex1.bfres')] ==
              900 + 3520 + hc.RSTB_MARGIN)  # (growth rounded up to 32)
        dgn = len(hc.sarc_replace(sarc({'Model/DgnMrgPrt_Dungeon000.Tex1.sbfres': tex(3, 500),
                                        'Actor/ActorLink/Dungeon000.bxml': b'1.9.0 actor'}),
                                  {'Model/DgnMrgPrt_Dungeon000.Tex1.sbfres': tex(14, 4000)}))
        check('table: a pack by name', n['Pack/Dungeon000.pack'] >= dgn)
        check('table: a DLC pack by its Aoc/0010/ name', n['Aoc/0010/Pack/RemainsElectric.pack'] > 2000)
        check('table: a file that did not grow stays', c[crc('Terrain/A/MainField.tscb')] == 100)

        hc.convert(game, pack, mod, terrain=False, fonts=False, log=lambda s: None)
        files = tree(out)
        check('--no-terrain leaves the terrain out', 'Terrain/A/MainField.tscb' not in files and
              'Model/Terrain.Tex1.sbfres' not in files and 'Model/Rock.Tex1.sbfres' in files)
        check('--no-fonts leaves the fonts out', 'Font/Font_US.sbfarc' not in files)
        c, _ = hc.rstb_read(open(os.path.join(out, hc.RSTB_PATH), 'rb').read())
        check('--no-terrain: its entries stay', c[crc('Model/Terrain.Tex1.bfres')] == 2000)

        hc.convert(game, pack, mod, copy=True, log=lambda s: None)
        src, dst = os.path.join(pack, 'Model/Rock.Tex1.sbfres'), os.path.join(out, 'Model/Rock.Tex1.sbfres')
        check('--copy copies instead of linking', not os.path.samefile(src, dst) and
              open(src, 'rb').read() == open(dst, 'rb').read())

    print('ok' if not failed else f'{failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
