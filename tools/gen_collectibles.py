"""Collectible hints on the map: the table runtime/collectibles_table.c builds from the game files, built here too, to
compare (make test-collectibles ROMFS=...).

    python tools/gen_collectibles.py extracted/romfs [table.txt]

Prints the table as tests/test_collectibles_table.c does, or compares it with that test's output (table.txt): each
tower region (its tower marker and its English name, the region numbers in the order of the towers' areas) and the flag
its tower's activation sets, each Korok seed and shrine (its region, its flags' hashes, where it is: world x, z), the
flag of the game's own count of the Koroks found, the regions' names in each language, and the region of each 100-unit
cell of the main field.

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
from gen_place_table import CELL, LANGUAGES, WORLD_X, WORLD_Z, msbt_texts, region_name, tower_grid

KOROKS = ('Npc_HiddenKorokGround', 'Npc_HiddenKorokFly')
KOROK_TOTAL, SHRINE_TOTAL, TOWER_TOTAL = 900, 120, 15


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    romfs = sys.argv[1]
    lines = []
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

    # The regions: each tower's area of MapTower.sbeco, numbered in the order of the areas
    static = byml(boot['Map/MainField/Static.smubin'])
    area = tower_grid(yaz0_decompress(boot['Ecosystem/MapTower.sbeco']))
    regions = {}
    for m in static['LocationMarker']:
        if m.get('Icon') == 'Tower':
            regions[area(m['Translate']['X'], m['Translate']['Z'])] = m['MessageID']
    if len(regions) != TOWER_TOTAL:
        sys.exit(f'{len(regions)} regions found, {TOWER_TOTAL} towers expected')
    ids = sorted(regions)

    def region_at(x, z):
        if not (-WORLD_X / 2 <= x < WORLD_X / 2 and -WORLD_Z / 2 <= z < WORLD_Z / 2):
            return -1
        a = area(x, z)
        return ids.index(a) if a in regions else -1

    def region(t, what):  # its region and where it is: (region, x, z) (a map unit's object: [x, y, z]; a marker: {X, Y, Z})
        x, z = (t[0], t[2]) if isinstance(t, list) else (t['X'], t['Z'])
        r = region_at(x, z)
        if r < 0:
            sys.exit(f'{what} at {x:.0f} {z:.0f}: in no tower region')
        return r, x, z

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

    def location_names(language):
        lang = sarc_files(open(os.path.join(romfs, 'Pack', f'Bootup_{language}.pack'), 'rb').read())
        msgs = sarc_files(yaz0_decompress(lang[f'Message/Msg_{language}.product.ssarc']))
        return msbt_texts(msgs['StaticMsg/LocationMarker.msbt'])

    names = location_names('USen')
    for r, a in enumerate(ids):
        n = regions[a]
        if not (n.startswith('Tower') and n[5:].isdigit()):
            sys.exit(f'{n}: a tower marker named TowerNN expected')
        lines.append(f'region {r} {n} {flag(f"MapTower_{n[5:]}", "bool_data"):08x} {region_name(names[n])}')
    for r, x, z, h in sorted(koroks.values()):
        lines.append(f'korok {r} {h:08x} {x:.1f} {z:.1f}')
    for r, x, z, found, cleared in sorted(shrines.values()):
        lines.append(f'shrine {r} {found:08x} {cleared:08x} {x:.1f} {z:.1f}')
    lines.append(f'total {flag("HiddenKorok_Number", "s32_data"):08x}')
    for language in LANGUAGES:  # (the regions' names in each language)
        texts = location_names(language)
        lines.append(f'names {language} ' + ' | '.join(region_name(texts[regions[a]]) for a in ids))
    for row in range(WORLD_Z // CELL):
        z = -WORLD_Z / 2 + (row + 0.5) * CELL
        cells = [region_at(-WORLD_X / 2 + (col + 0.5) * CELL, z) for col in range(WORLD_X // CELL)]
        lines.append('grid ' + ''.join(chr(ord('a') + r) if r >= 0 else '.' for r in cells))
    if len(sys.argv) < 3:
        print('\n'.join(lines))
        return
    theirs = open(sys.argv[2], encoding='utf-8').read().splitlines()
    for i in range(max(len(lines), len(theirs))):
        ours, other = lines[i] if i < len(lines) else '', theirs[i] if i < len(theirs) else ''
        if ours != other:
            sys.exit(f'line {i + 1}: {other!r} in {sys.argv[2]}, {ours!r} here')
    print(f'the same table: {len(lines)} lines')


if __name__ == '__main__':
    main()
