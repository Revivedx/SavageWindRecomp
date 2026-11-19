"""Collectible hints on the map, from your own game files: writes runtime/collectibles_table.inc.

    python tools/gen_collectibles.py extracted/romfs [runtime/collectibles_table.inc]

The Makefile runs it: the table is made from the game's own data, so it is never committed. For each Korok seed and
each shrine, the game data flag that says it is found (and for a shrine, cleared), where it is (world x, z: for the
map's markers) and the tower region it is in, in the order of game_place_table.inc's region_names
(tools/gen_place_table.py tower_regions).

- Korok seeds: the hidden Koroks (Npc_HiddenKorokGround, Npc_HiddenKorokFly) of the main field's map units
  (Map/MainField/<unit>/<unit>_Static.smubin, _Dynamic.smubin); each sets its own bool flag
  MainField_<actor>_<HashId> when found.
- Shrines: the shrine markers of Map/MainField/Static.smubin (Pack/Bootup.pack); Location_DungeonNNN (s32, not 0 once
  found) and Clear_DungeonNNN (bool).
- Towers: MapTower_NN (bool), set when the tower of the region is activated (its marker in Static.smubin is TowerNN; the
  marker's own Location_MapTowerNN only says it was seen). The map shows hints only in those regions.
- HiddenKorok_Number (s32): the game's own count of the Koroks found, which the runtime checks its reading against.

A flag's hash is the one the game keeps with it (gamedata's HashValue, the crc32 of its name).
"""
import glob
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from botw_save_convert import Byml, sarc_files, yaz0_decompress
from gen_place_table import tower_regions

KOROKS = ('Npc_HiddenKorokGround', 'Npc_HiddenKorokFly')
KOROK_TOTAL, SHRINE_TOTAL = 900, 120


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    romfs = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'runtime', 'collectibles_table.inc')
    boot = sarc_files(open(os.path.join(romfs, 'Pack', 'Bootup.pack'), 'rb').read())

    def byml(data):
        b = Byml(yaz0_decompress(data))
        return b._node(b.root)

    flags = {}  # name: (kind, hash)
    for blob in sarc_files(yaz0_decompress(boot['GameData/gamedata.ssarc'])).values():
        for kind, entries in Byml(blob).parse().items():
            for e in entries:
                flags[e['DataName']] = (kind, e['HashValue'] & 0xFFFFFFFF)

    def flag(name, kind):
        if name not in flags:
            sys.exit(f'{name}: no such flag in gamedata')
        k, h = flags[name]
        if k != kind or h != zlib.crc32(name.encode()):
            sys.exit(f'{name}: a {k} with hash {h:08x}, a {kind} with the crc32 of its name expected')
        return h

    static = byml(boot['Map/MainField/Static.smubin'])
    area, regions, ids = tower_regions(static, yaz0_decompress(boot['Ecosystem/MapTower.sbeco']))

    def region(t, what):  # its region and where it is: (region, x, z) (a map unit's object: [x, y, z]; a marker: {X, Y, Z})
        x, z = (t[0], t[2]) if isinstance(t, list) else (t['X'], t['Z'])
        a = area(x, z)
        if a not in regions:
            sys.exit(f'{what} at {x:.0f} {z:.0f}: in no tower region')
        return ids.index(a), x, z

    koroks = {}
    for path in sorted(glob.glob(os.path.join(romfs, 'Map', 'MainField', '*', '*_*.smubin'))):
        if not path.endswith(('_Static.smubin', '_Dynamic.smubin')):
            continue
        for o in byml(open(path, 'rb').read()).get('Objs', []):
            if o.get('UnitConfigName') in KOROKS:
                name = f'MainField_{o["UnitConfigName"]}_{o["HashId"]}'
                koroks[name] = (*region(o['Translate'], name), flag(name, 'bool_data'))
    if len(koroks) != KOROK_TOTAL:
        sys.exit(f'{len(koroks)} Korok seeds found, {KOROK_TOTAL} expected')

    shrines = {}
    for m in static['LocationMarker']:
        if m.get('Icon') == 'Dungeon':
            n = m['MessageID']  # DungeonNNN
            shrines[n] = (*region(m['Translate'], n), flag(f'Location_{n}', 's32_data'), flag(f'Clear_{n}', 'bool_data'))
    if len(shrines) != SHRINE_TOTAL:
        sys.exit(f'{len(shrines)} shrines found, {SHRINE_TOTAL} expected')

    towers = []  # by region
    for a in ids:
        n = regions[a]
        if not (n.startswith('Tower') and n[5:].isdigit()):
            sys.exit(f'{n}: a tower marker named TowerNN expected')
        towers.append(flag(f'MapTower_{n[5:]}', 'bool_data'))

    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Made by tools/gen_collectibles.py from the game files: do not edit.\n')
        f.write(f'#define COLLECT_REGIONS {len(ids)}  // (the regions of game_place_table.inc, in its order)\n')
        f.write(f'#define COLLECT_KOROKS_FOUND 0x{flag("HiddenKorok_Number", "s32_data"):08x}u  // the game\'s own count (s32)\n\n')
        f.write('// Korok seeds: region, the flag found sets, where (x, z)\nstatic const CollectKorok korok_table[] = {\n')
        for r, x, z, h in sorted(koroks.values()):
            f.write(f'    {{{r}, 0x{h:08x}u, {x:.1f}f, {z:.1f}f}},\n')
        f.write('};\n\n// Shrines: region, found (s32, not 0), cleared (bool), where (x, z)\nstatic const CollectShrine shrine_table[] = {\n')
        for r, x, z, found, cleared in sorted(shrines.values()):
            f.write(f'    {{{r}, 0x{found:08x}u, 0x{cleared:08x}u, {x:.1f}f, {z:.1f}f}},\n')
        f.write('};\n\n// Towers, by region: the flag its activation sets (bool)\n'
                'static const uint32_t tower_table[COLLECT_REGIONS] = {\n')
        for h in towers:
            f.write(f'    0x{h:08x}u,\n')
        f.write('};\n')
    print(f'{len(koroks)} Korok seeds, {len(shrines)} shrines, {len(ids)} regions -> {out}')


if __name__ == '__main__':
    main()
