#!/usr/bin/env python3
"""Make a texture pack made for the game's 1.6.0 update into a Savage Wind mod for 1.9.0, from your own game files.

    python3 tools/hdpack_convert.py <game files folder> <pack RomFS folder> <mod folder> [--no-terrain] [--no-fonts]
                                    [--copy]

    <game files folder>  the folder with romfs/ (and the DLC's aoc1/ if you have it): the one Savage Wind uses
    <pack RomFS folder>  the pack's RomFS folder (Model/, Pack/, Terrain/, ...)
    <mod folder>         where the mod is made, for example mods/HDPack in Savage Wind's data folder; it gets a
                         romfs/ folder. An existing romfs/ in it is replaced.

A texture pack made for 1.6.0 (such as HDPacks.com's 4K pack) can't be loaded on 1.9.0 as it is: next to its
textures it has 1.6.0 copies of whole packs (the title screen, the Divine Beasts, actors), Pack/Bootup.pack and a
1.6.0 ResourceSizeTable, and the game crashes or never finishes loading. This makes a mod of only its textures, on
top of your 1.9.0 files:

- loose files (Model/*.Tex*.sbfres, UI/StockItem/..., Terrain/...) that 1.9.0 or its DLC also has: linked (hard links
  on the same drive, copies otherwise or with --copy); files 1.9.0 doesn't have are left out
- Pack/*.pack: your 1.9.0 pack with only its texture files (*.Tex*.sbfres) taken from the pack's copy. The DLC's
  packs (the shrines of The Master Trials and The Champions' Ballad) go under aoc/0010/. Pack/Bootup.pack is left
  out (1.6.0 game data; Savage Wind patches its own)
- Font/Font_US.sbfarc: your 1.9.0 archive with the pack's fonts, except External_00.bffnt (the button icons, which
  Savage Wind draws itself). --no-fonts leaves the fonts out
- System/Resource/ResourceSizeTable.product.srsizetable: your 1.9.0 table with the entries of the files that grew
  made bigger: the largest of the 1.9.0 size, the pack's own table's size, and the 1.9.0 size plus what the file grew
  by. The game sets aside memory by this table, and Savage Wind's own growing does not reach the files inside packs

A pack this big needs more game memory than the game's own: set game_memory_mb = 6144 in savagewind.ini. Without it the
textures overflow the game's texture pool (walls and floors missing in shrines), and the 4K terrain (Terrain/ and
Model/Terrain.Tex*.sbfres, which make Pack/TitleBG.pack 80 -> 287 MB) crashes the title screen. --no-terrain leaves the
terrain out. Textures this big also need a GPU with enough memory.

The mod is made from your game files: it holds parts of them (the packs) and must not be shared. Nothing is
downloaded and nothing outside <mod folder>/romfs is written.
"""
import os
import shutil
import struct
import sys
import zlib

RSTB_PATH = 'System/Resource/ResourceSizeTable.product.srsizetable'
RSTB_MARGIN = 0x1000  # (room on top of what a file grew by, as the pack's own table leaves)
SKIP_DIRS = ('Pack', 'System', 'Font')  # (handled on their own)


# ---- Yaz0 -------------------------------------------------------------------------------------------------------

def yaz0_size(head):
    """The decompressed size of a file from its first 16 bytes (its own size when it isn't Yaz0)."""
    return struct.unpack_from('>I', head, 4)[0] if head[:4] == b'Yaz0' else None


def yaz0_decompress(d):
    if d[:4] != b'Yaz0':
        return d
    size, = struct.unpack_from('>I', d, 4)
    out = bytearray(size)
    i, o = 16, 0
    while o < size:
        code = d[i]
        i += 1
        for bit in range(8):
            if o >= size:
                break
            if code & (0x80 >> bit):
                out[o] = d[i]
                i += 1
                o += 1
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
                s = o - dist
                if dist >= n:
                    out[o:o + n] = out[s:s + n]
                else:  # (overlapping: a run)
                    for k in range(n):
                        out[o + k] = out[s + k]
                o += n
    return bytes(out)


def yaz0_store(d):
    """Yaz0 without compression (every byte a literal): the game reads it like any other."""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(d)) + bytes(8))
    for i in range(0, len(d), 8):
        chunk = d[i:i + 8]
        out.append((0xFF00 >> len(chunk)) & 0xFF)
        out += chunk
    return bytes(out)


# ---- SARC (little endian, as on the Switch) ----------------------------------------------------------------------

def _sarc_entries(d):
    if d[:4] != b'SARC' or d[6:8] != b'\xff\xfe':
        raise ValueError('not a little-endian SARC archive')
    hdr_len, = struct.unpack_from('<H', d, 4)
    data_off, = struct.unpack_from('<I', d, 0x0C)
    sfat = hdr_len
    if d[sfat:sfat + 4] != b'SFAT':
        raise ValueError('SARC: SFAT missing')
    n, = struct.unpack_from('<H', d, sfat + 6)
    nodes = sfat + 12
    sfnt = nodes + n * 16
    if d[sfnt:sfnt + 4] != b'SFNT':
        raise ValueError('SARC: SFNT missing')
    names = sfnt + 8
    out = []
    for i in range(n):
        h, attr, s, e = struct.unpack_from('<IIII', d, nodes + i * 16)
        name = '%08x' % h
        if attr & 0x01000000:
            o = names + (attr & 0xFFFF) * 4
            name = d[o:d.index(b'\0', o)].decode()
        out.append((i, name, s, e))
    return data_off, nodes, out


def sarc_files(d):
    """{name: bytes} of a SARC archive."""
    data_off, _, entries = _sarc_entries(d)
    return {name: d[data_off + s:data_off + e] for _, name, s, e in entries}


def sarc_replace(d, new):
    """The archive d with the files named in new ({name: bytes}) replaced. Its tables (order, hashes, names) stay;
    each file's data starts at the alignment its old start had (the largest power of two dividing it, up to 0x2000)."""
    data_off, nodes, entries = _sarc_entries(d)
    missing = set(new) - {name for _, name, _, _ in entries}
    if missing:
        raise KeyError(f'not in the archive: {sorted(missing)[:5]}')
    head = bytearray(d[:data_off])
    body = bytearray()
    for i, name, s, e in sorted(entries, key=lambda x: x[2]):  # (data in its old order)
        align = 0x2000
        while align > 1 and (data_off + s) % align:
            align >>= 1
        blob = new.get(name, d[data_off + s:data_off + e])
        body += bytes(-(data_off + len(body)) % align)
        start = len(body)
        body += blob
        struct.pack_into('<II', head, nodes + i * 16 + 8, start, start + len(blob))
    out = head + body
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


# ---- ResourceSizeTable -----------------------------------------------------------------------------------------

def rstb_name(path):
    """A RomFS path as the table names it: a Yaz0 file's extension loses its leading 's' (.sarc keeps it); the DLC's
    files are under Aoc/0010/."""
    if path.startswith('aoc/0010/'):
        path = 'Aoc/0010/' + path[len('aoc/0010/'):]
    stem, dot, ext = path.rpartition('.')
    if dot and ext.startswith('s') and ext != 'sarc':
        return f'{stem}.{ext[1:]}'
    return path


def rstb_read(d):
    d = yaz0_decompress(d)
    if d[:4] != b'RSTB':
        raise ValueError('not a ResourceSizeTable')
    ncrc, nname = struct.unpack_from('<II', d, 4)
    crc = dict(struct.unpack_from('<II', d, 12 + i * 8) for i in range(ncrc))
    names, o = {}, 12 + ncrc * 8
    for _ in range(nname):
        names[d[o:o + 128].split(b'\0')[0].decode()], = struct.unpack_from('<I', d, o + 128)
        o += 132
    return crc, names


def rstb_write(crc, names):
    out = bytearray(b'RSTB' + struct.pack('<II', len(crc), len(names)))
    for c in sorted(crc):
        out += struct.pack('<II', c, crc[c])
    for n, v in names.items():
        out += n.encode().ljust(128, b'\0') + struct.pack('<I', v)
    return yaz0_store(bytes(out))


class SizeTable:
    def __init__(self, game, pack):
        self.crc, self.names = rstb_read(game)
        self.pcrc, self.pnames = rstb_read(pack) if pack else ({}, {})
        self.grown = 0

    def grow(self, path, by):
        """The entry of path made room for a file by bytes bigger (decompressed) than 1.9.0's."""
        n = rstb_name(path)
        if n in self.names:
            tab, key, ptab = self.names, n, self.pnames
        else:
            tab, key, ptab = self.crc, zlib.crc32(n.encode()), self.pcrc
        if key not in tab:
            return
        want = max(tab[key], ptab.get(key, 0), tab[key] + ((by + 31) & ~31) + RSTB_MARGIN if by > 0 else 0)
        if want > tab[key]:
            tab[key] = want
            self.grown += 1


# ---- the mod ---------------------------------------------------------------------------------------------------

def head16(path):
    with open(path, 'rb') as f:
        return f.read(16)


def unpacked_size(path):
    s = yaz0_size(head16(path))
    return os.path.getsize(path) if s is None else s


def place(src, dst, copy):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not copy:
        try:
            os.link(src, dst)
            return
        except OSError:
            pass  # (another drive, or no hard links there)
    shutil.copyfile(src, dst)


def is_terrain(rel):
    return rel.startswith('Terrain/') or rel.startswith('Model/Terrain.Tex')


def convert(game, pack, mod, terrain=True, fonts=True, copy=False, log=print):
    romfs, aoc = os.path.join(game, 'romfs'), os.path.join(game, 'aoc1')
    if not os.path.isfile(os.path.join(romfs, RSTB_PATH)):
        raise SystemExit(f'{game}: no romfs/{RSTB_PATH} (the folder with your romfs/ and aoc1/)')
    out = os.path.join(mod, 'romfs')
    if os.path.isdir(out):
        shutil.rmtree(out)
    pack_rstb = os.path.join(pack, RSTB_PATH)
    with open(os.path.join(romfs, RSTB_PATH), 'rb') as f:
        game_rstb = f.read()
    table = SizeTable(game_rstb, open(pack_rstb, 'rb').read() if os.path.isfile(pack_rstb) else None)

    def base_of(rel):
        """(1.9.0 file, its path in the mod) for a RomFS path, from the game or its DLC; None if neither has it."""
        if os.path.isfile(os.path.join(romfs, rel)):
            return os.path.join(romfs, rel), rel
        if os.path.isfile(os.path.join(aoc, rel)):
            return os.path.join(aoc, rel), 'aoc/0010/' + rel
        return None

    # loose files
    linked = left = 0
    for top in sorted(os.listdir(pack)):
        if top in SKIP_DIRS or not os.path.isdir(os.path.join(pack, top)):
            continue
        for dp, _, fs in os.walk(os.path.join(pack, top)):
            for f in sorted(fs):
                src = os.path.join(dp, f)
                rel = os.path.relpath(src, pack).replace(os.sep, '/')
                base = base_of(rel)
                if base is None or (not terrain and is_terrain(rel)):
                    left += 1
                    continue
                place(src, os.path.join(out, base[1]), copy)
                table.grow(base[1], unpacked_size(src) - unpacked_size(base[0]))
                linked += 1
    log(f'loose files: {linked} linked, {left} left out (not in 1.9.0{"" if terrain else ", or terrain"})')

    # packs
    n_packs = n_tex = 0
    pack_dir = os.path.join(pack, 'Pack')
    for f in sorted(os.listdir(pack_dir)) if os.path.isdir(pack_dir) else ():
        rel = 'Pack/' + f
        base = base_of(rel)
        if f == 'Bootup.pack' or base is None:
            continue
        with open(base[0], 'rb') as fh:
            a_raw = fh.read()
        with open(os.path.join(pack_dir, f), 'rb') as fh:
            b = sarc_files(yaz0_decompress(fh.read()))
        a_arc = yaz0_decompress(a_raw)
        a = sarc_files(a_arc)
        new = {k: v for k, v in b.items() if k in a and a[k] != v and '.Tex' in k and k.endswith('.sbfres') and
               (terrain or not k.startswith('Model/Terrain.Tex'))}
        if not new:
            continue
        for k, v in new.items():
            table.grow(k, (yaz0_size(v[:16]) or len(v)) - (yaz0_size(a[k][:16]) or len(a[k])))
        built = sarc_replace(a_arc, new)
        dst = os.path.join(out, base[1])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as fh:
            fh.write(built if a_raw[:4] != b'Yaz0' else yaz0_store(built))
        table.grow(base[1], len(built) - len(a_arc))
        n_packs += 1
        n_tex += len(new)
    log(f'packs: {n_packs} of your 1.9.0 packs with {n_tex} of the pack\'s textures')

    # fonts
    rel = 'Font/Font_US.sbfarc'
    if fonts and os.path.isfile(os.path.join(pack, rel)) and os.path.isfile(os.path.join(romfs, rel)):
        with open(os.path.join(romfs, rel), 'rb') as fh:
            a_raw = fh.read()
        a_arc = yaz0_decompress(a_raw)
        a = sarc_files(a_arc)
        with open(os.path.join(pack, rel), 'rb') as fh:
            b = sarc_files(yaz0_decompress(fh.read()))
        new = {k: v for k, v in b.items() if k in a and a[k] != v and k != 'External_00.bffnt'}
        if new:
            built = sarc_replace(a_arc, new)
            os.makedirs(os.path.join(out, 'Font'), exist_ok=True)
            with open(os.path.join(out, rel), 'wb') as fh:
                fh.write(yaz0_store(built))
            table.grow(rel, len(built) - len(a_arc))
            for k, v in new.items():
                table.grow('Font/' + k, len(v) - len(a[k]))
            log(f'fonts: {", ".join(sorted(new))} (External_00.bffnt kept)')

    os.makedirs(os.path.dirname(os.path.join(out, RSTB_PATH)), exist_ok=True)
    with open(os.path.join(out, RSTB_PATH), 'wb') as fh:
        fh.write(rstb_write(table.crc, table.names))
    log(f'ResourceSizeTable: {table.grown} entries grown')
    return table.grown


def main(argv):
    flags = {a for a in argv if a.startswith('--')}
    args = [a for a in argv if not a.startswith('--')]
    unknown = flags - {'--no-terrain', '--no-fonts', '--copy'}
    if len(args) != 3 or unknown:
        sys.exit(__doc__)
    game, pack, mod = args
    convert(game, pack, mod, terrain='--no-terrain' not in flags, fonts='--no-fonts' not in flags,
            copy='--copy' in flags)
    print(f'made {os.path.join(mod, "romfs")}. Turn it on in the Mods tab, and set game_memory_mb = 6144 in '
          'savagewind.ini.')


if __name__ == '__main__':
    main(sys.argv[1:])
