#!/usr/bin/env python3
"""The progress bars of README.md and docs/STATUS.md, from the tables in docs/STATUS.md.

Each "### Area (N%)" heading gives the area's share of the whole. Each row of its table gives a part's progress and
its weight inside the area (x1 an extra, x2 important, x3 essential). An area's value is the weighted mean of its
parts, and the overall value the weighted mean of the areas. Run it after changing the table:

    python tools/status_bars.py
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
STATUS = os.path.join(ROOT, 'docs', 'STATUS.md')
OUT = os.path.join(ROOT, 'docs', 'media', 'progress')

AREA = re.compile(r'^### (.+?) \((\d+)%\)\s*$')
ROW = re.compile(r'^\|[^|]*\|\s*\*\*(.+?)\*\*\s*\|\s*!\[(\d+)%\]\(media/progress/(\d+)\.svg\)\s*\|\s*x([123])\s*\|')


def colour(pct):
    return '#5cb85c' if pct >= 70 else '#f0ad4e' if pct >= 30 else '#d9534f'


def bar(name, pct, width, height, label):
    fill = round(width * pct / 100)
    size = 11 if height <= 20 else 13
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
           f'role="img" aria-label="{label}"><rect width="{width}" height="{height}" fill="#6e7681"/>'
           f'<rect width="{fill}" height="{height}" fill="{colour(pct)}"/>'
           f'<text x="{width // 2}" y="{height // 2 + 4}" text-anchor="middle" '
           f'font-family="Verdana,DejaVu Sans,sans-serif" font-size="{size}" fill="#fff">{label}</text></svg>\n')
    with open(os.path.join(OUT, name + '.svg'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(svg)


def read():
    areas, cur = [], None
    with open(STATUS, encoding='utf-8') as f:
        for line in f:
            m = AREA.match(line)
            if m:
                cur = {'name': m[1], 'share': int(m[2]), 'parts': []}
                areas.append(cur)
                continue
            m = ROW.match(line)
            if m and cur:
                if m[2] != m[3]:
                    sys.exit(f'{m[1]}: the bar says {m[2]}% and its picture {m[3]}%')
                cur['parts'].append((m[1], int(m[2]), int(m[4])))
    return areas


def main():
    areas = read()
    if not areas or sum(a['share'] for a in areas) != 100:
        sys.exit('the areas must add up to 100%')
    total = 0.0
    for a in areas:
        if not a['parts']:
            sys.exit(f"{a['name']}: no parts found")
        a['value'] = sum(p * w for _, p, w in a['parts']) / sum(w for _, _, w in a['parts'])
        total += a['value'] * a['share'] / 100
        for _, p, _w in a['parts']:
            bar(str(p), p, 150, 20, f'{p}%')
        slug = re.sub(r'[^a-z0-9]+', '-', a['name'].lower()).strip('-')
        bar('area-' + slug, round(a['value']), 220, 20, f"{round(a['value'])}%")
        print(f"{a['share']:3d}%  {a['value']:5.1f}%  {a['name']} ({len(a['parts'])} parts)  area-{slug}.svg")
    bar('overall', round(total), 420, 28, f'{round(total)}% overall')
    print(f'overall {total:.1f}%')


if __name__ == '__main__':
    main()
