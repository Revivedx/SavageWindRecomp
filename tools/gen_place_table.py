"""The place names of Discord Rich Presence, from your own game files: writes runtime/game_place_table.inc.

    python tools/gen_place_table.py extracted/romfs/Pack [runtime/game_place_table.inc]

The Makefile runs it: the table is the game's own text, so it is never committed. From Pack/Bootup.pack: the map's labels (Map/MainField/Location.smubin),
the places with an icon and the small landmarks (LocationMarker, LocationPointer in Map/MainField/Static.smubin), and
the tower regions (Ecosystem/MapTower.sbeco). From each language's Pack/Bootup_<language>.pack: their names.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from botw_save_convert import Byml, sarc_files, yaz0_decompress

# How far from its point a name holds, by kind. The closest name relative to its radius wins, so a village's wide
# radius beats a hill's next to it, and a stable's short one only holds at the stable.
TOWN_ICONS = ('Village', 'Castle')  # inside these, their name beats any label nearby
MARKER_RADIUS = {'Village': 320, 'Castle': 420, 'Hatago': 130, 'CheckPoint': 170, 'Labo': 90, 'StartPoint': 120}
POINTER_RADIUS = 90
LABEL_RADIUS = {2: 750, 4: 380, 8: 210}  # by ShowLevel: the map's biggest labels are 2
LABEL_REGION_TYPE = 6  # (Eldin, Hebra...: the region line already says it)

# Each language's text the game has no message for: main menu, exploring, inside a shrine, Divine Beast, inside, and a
# shrine with no name of its own (Discord refuses a line of one character: 試練の祠, not 祠)
TEXTS = {
    'USen': ('In the main menu', 'Exploring', 'Inside a shrine', 'Divine Beast', 'Interior', 'Shrine'),
    'EUen': ('In the main menu', 'Exploring', 'Inside a shrine', 'Divine Beast', 'Interior', 'Shrine'),
    'USes': ('En el menú principal', 'Explorando', 'Interior de santuario', 'Bestia Divina', 'Interior', 'Santuario'),
    'EUes': ('En el menú principal', 'Explorando', 'Interior de santuario', 'Bestia Divina', 'Interior', 'Santuario'),
    'USfr': ('Au menu principal', 'Exploration', 'Dans un sanctuaire', 'Créature divine', 'Intérieur', 'Sanctuaire'),
    'EUfr': ('Au menu principal', 'Exploration', 'Dans un sanctuaire', 'Créature divine', 'Intérieur', 'Sanctuaire'),
    'EUde': ('Im Hauptmenü', 'Auf Erkundung', 'In einem Schrein', 'Titan', 'Innenraum', 'Schrein'),
    'EUit': ('Nel menu principale', 'In esplorazione', 'In un sacrario', 'Colosso sacro', 'Interno', 'Sacrario'),
    'EUnl': ('In het hoofdmenu', 'Op verkenning', 'In een tempel', 'Goddelijk beest', 'Interieur', 'Tempel'),
    'EUru': ('В главном меню', 'Исследует мир', 'В святилище', 'Чудище', 'Внутри', 'Святилище'),
    'USpt': ('No menu principal', 'Explorando', 'Em um santuário', 'Fera Divina', 'Interior', 'Santuário'),
    'JPja': ('メインメニュー', '探索中', '祠の中', '神獣', '屋内', '試練の祠'),
    'KRko': ('메인 메뉴', '탐험 중', '사당 내부', '신수', '실내', '사당'),
    'CNzh': ('主菜单', '探索中', '神庙内', '神兽', '室内', '神庙'),
    'TWzh': ('主選單', '探索中', '神廟內', '神獸', '室內', '神廟'),
    'THth': ('เมนูหลัก', 'กำลังสำรวจ', 'ภายในเทวสถาน', 'อสูรเทพ', 'ภายใน', 'เทวสถาน'),
}
LANGUAGES = list(TEXTS)

WORLD_X, WORLD_Z = 10000, 8000  # x -5000..5000, z -4000..4000
CELL = 100  # the region grid: 100 x 80 cells


def clean_text(s):
    """A message without its control tags (0x0e group tag size, then size bytes of data) and final NUL."""
    out, i = [], 0
    while i < len(s):
        if s[i] == '\x0e':
            i += 4 + ord(s[i + 3]) // 2
        elif s[i] == '\0':
            break
        elif s[i] == '\n':  # (a long name, broken to fit the map: a space, except between CJK characters)
            if out and out[-1] < '⺀':
                out.append(' ')
            i += 1
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


def msbt_texts(d):
    """{label: text} of an MSBT file."""
    if d[:8] != b'MsgStdBn':
        raise ValueError('not an MSBT file')
    nsec, = struct.unpack_from('<H', d, 0xE)
    sec, o = {}, 0x20
    for _ in range(nsec):
        size, = struct.unpack_from('<I', d, o + 4)
        sec[d[o:o + 4]] = d[o + 16:o + 16 + size]
        o = (o + 16 + size + 15) & ~15
    lbl, txt = sec[b'LBL1'], sec[b'TXT2']
    nbuckets, = struct.unpack_from('<I', lbl, 0)
    ntxt, = struct.unpack_from('<I', txt, 0)
    out = {}
    for b in range(nbuckets):
        n, off = struct.unpack_from('<II', lbl, 4 + 8 * b)
        for _ in range(n):
            ln = lbl[off]
            name = lbl[off + 1:off + 1 + ln].decode()
            idx, = struct.unpack_from('<I', lbl, off + 1 + ln)
            off += ln + 5
            start, = struct.unpack_from('<I', txt, 4 + 4 * idx)
            end = struct.unpack_from('<I', txt, 8 + 4 * idx)[0] if idx + 1 < ntxt else len(txt)
            out[name] = clean_text(txt[start:end].decode('utf-16le', 'replace'))
    return out


def tower_grid(d):
    """The tower area id at (x, z), from MapTower.sbeco: a row of runs (u16 area, u16 length) per 2 units of z, offsets
    to them in u16s."""
    magic, nrows, row_h, _ = struct.unpack_from('<4I', d, 0)
    if magic != 0x00112233:
        raise ValueError('not an ecosystem file')
    rows = 0x10 + 4 * nrows

    def area(x, z):
        row = min(max(int((z + WORLD_Z / 2) / row_h), 0), nrows - 1)
        o = rows + 2 * struct.unpack_from('<I', d, 0x10 + 4 * row)[0]
        left = min(max(int(x + WORLD_X / 2), 0), WORLD_X - 1)
        while True:
            a, n = struct.unpack_from('<HH', d, o)
            if left < n:
                return a
            left -= n
            o += 4
    return area


def region_name(tower):
    """"Torre del lago" -> "Lago"; "Torre de Hebra" -> "Hebra"; "Lake Tower" -> "Lake". Other languages build the name in
    too many ways (Hebra-Turm, Turm der Ebene): there it stays the tower's."""
    if tower.endswith(' Tower'):
        return tower[:-len(' Tower')]
    for p in ('Torre de las ', 'Torre de la ', 'Torre del ', 'Torre de '):
        if tower.startswith(p):
            s = tower[len(p):]
            return s[0].upper() + s[1:]
    return tower


def beast_name(name, word):
    """"Bestia divina Vah Ruta" -> "Vah Ruta"; the second line already says what it is."""
    return name[len(word) + 1:] if name.lower().startswith(word.lower() + ' ') else name


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def c_row(texts):
    if min(len(t) for t in texts) < 2:
        sys.exit(f'a line of one character: {texts}')
    return '{' + ', '.join(c_str(t) for t in texts) + '}'


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pack = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'runtime', 'game_place_table.inc')
    boot = sarc_files(open(os.path.join(pack, 'Bootup.pack'), 'rb').read())

    def byml(name):
        b = Byml(yaz0_decompress(boot[name]))
        return b._node(b.root)

    def location_names(region):
        lang = sarc_files(open(os.path.join(pack, f'Bootup_{region}.pack'), 'rb').read())
        msgs = sarc_files(yaz0_decompress(lang[f'Message/Msg_{region}.product.ssarc']))
        return msbt_texts(msgs['StaticMsg/LocationMarker.msbt'])

    texts = [location_names(language) for language in LANGUAGES]
    names = texts[0]

    def in_all(msg):
        return [t[msg] for t in texts]

    static = byml('Map/MainField/Static.smubin')
    places = []
    for m in static['LocationMarker']:
        r = MARKER_RADIUS.get(m.get('Icon'))
        if r and m.get('MessageID') in names:
            places.append((m['Translate']['X'], m['Translate']['Z'], r, m['MessageID'], m.get('Icon') in TOWN_ICONS))
    for p in static['LocationPointer']:
        if p.get('MessageID') in names:
            places.append((p['Translate']['X'], p['Translate']['Z'], POINTER_RADIUS, p['MessageID'], False))
    for l in byml('Map/MainField/Location.smubin'):
        if l['Type'] != LABEL_REGION_TYPE and l['MessageID'] in names:
            places.append((l['Translate']['X'], l['Translate']['Z'], LABEL_RADIUS[l['ShowLevel']], l['MessageID'], False))

    # The regions: each area of the grid is named by the tower that stands in it
    area = tower_grid(yaz0_decompress(boot['Ecosystem/MapTower.sbeco']))
    regions = {}
    for m in static['LocationMarker']:
        if m.get('Icon') == 'Tower':
            regions[area(m['Translate']['X'], m['Translate']['Z'])] = m['MessageID']
    if len(regions) != 15:
        sys.exit(f'{len(regions)} regions found, 15 towers expected')
    ids = sorted(regions)
    grid = []
    for row in range(WORLD_Z // CELL):
        z = -WORLD_Z / 2 + (row + 0.5) * CELL
        cells = [area(-WORLD_X / 2 + (col + 0.5) * CELL, z) for col in range(WORLD_X // CELL)]
        grid.append(''.join(chr(ord('a') + ids.index(a)) if a in regions else '.' for a in cells))

    # The interiors, by pack. The add-on content's shrines (120...) have no name in the game's own message files.
    interiors = []
    for k in range(120):
        interiors.append((f'Dungeon{k:03}', in_all(f'Dungeon{k:03}'), 'SHRINE'))
    for k in range(120, 137):
        interiors.append((f'Dungeon{k:03}', [TEXTS[language][5] for language in LANGUAGES], 'SHRINE'))
    for pack_name in ('RemainsWater', 'RemainsFire', 'RemainsWind', 'RemainsElectric'):
        interiors.append((pack_name, [beast_name(t[pack_name], TEXTS[language][3]) for language, t in zip(LANGUAGES, texts)], 'BEAST'))
    interiors.append(('FinalTrial', in_all('FinalTrial'), 'TRIAL'))

    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Made by tools/gen_place_table.py from the game files: do not edit.\n')
        f.write('static const char *const place_languages[PLACE_LANGUAGES] = {' + ', '.join(c_str(l) for l in LANGUAGES) + '};\n\n')
        f.write('// main menu, exploring, inside a shrine, Divine Beast, inside\n')
        f.write('static const char *const place_texts[PLACE_LANGUAGES][5] = {\n')
        for language in LANGUAGES:
            f.write(f'    {c_row(TEXTS[language][:5])},\n')
        f.write('};\n\nstatic const PlaceEntry place_table[] = {\n')
        for x, z, r, msg, town in sorted(places, key=lambda p: (p[0], p[1])):
            f.write(f'    {{{x:.0f}, {z:.0f}, {r}, {str(town).lower()}, {c_row(in_all(msg))}}},\n')
        f.write('};\n\nstatic const char *const region_names[][PLACE_LANGUAGES] = {\n')
        for i in ids:
            f.write(f'    {c_row([region_name(t) for t in in_all(regions[i])])},\n')
        f.write('};\n\n// A row per 100 units of z from -4000, a cell per 100 of x from -5000: a letter of region_names, . none\n')
        f.write(f'static const char region_grid[{len(grid)}][{len(grid[0]) + 1}] = {{\n')
        for g in grid:
            f.write(f'    "{g}",\n')
        f.write('};\n\nstatic const InteriorEntry interior_table[] = {\n')
        for pack_name, row, kind in interiors:
            f.write(f'    {{{c_str(pack_name)}, {c_row(row)}, INTERIOR_{kind}}},\n')
        f.write('};\n')
    print(f'{len(places)} places, {len(ids)} regions, {len(interiors)} interiors -> {out}')


if __name__ == '__main__':
    main()
