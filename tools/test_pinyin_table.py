#!/usr/bin/env python3
"""runtime/pinyin_data.inc: the table is intact, and a few known characters read what they should.

The `.inc` is generated (tools/gen_pinyin_table.py) from the Unicode Han Database, so it can't be regenerated here
without that file: this checks the committed table instead - sorted codepoints, readings inside the pool and plain a-z,
no reading twice - and the readings of a handful of ordinary characters. It is ordinary Chinese (鱼 yu, 鲈 lu), not
the game's text.

usage: tools/test_pinyin_table.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, 'runtime/pinyin_data.inc')

# What a few characters of the table read, and so what a player can type to find them
KNOWN = {'\u9c7c': 'yu', '\u9c88': 'lu', '\u5973': 'nu', '\u70e4': 'kao', '\u51b0': 'bing', '\u4e00': 'yi'}

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)


def numbers(name, text):
    """The values of the array `name` in the table."""
    m = re.search(r'%s\[\]\s*=\s*\{(.*?)\};' % re.escape(name), text, re.S)
    if not m:
        raise SystemExit('%s: no %s' % (TABLE, name))
    return [int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(1))]


def readings(text):
    """The readings pool, as a list of (offset, reading), and its size in bytes."""
    m = re.search(r'g_pinyin_readings\[\]\s*=\s*(.*?);', text, re.S)
    if not m:
        raise SystemExit('%s: no g_pinyin_readings' % TABLE)
    body = re.sub(r'["\s]', '', m.group(1))  # (the pool is plain a-z, a \\0 between the readings)
    check('\\' not in body.replace('\\0', ''), 'the pool holds an escape other than \\0')
    words = body.split('\\0')
    check(words and words[-1] == '', 'the pool does not end with \\0')
    out, at = [], 0
    for word in words[:-1]:
        out.append((at, word))
        at += len(word) + 1
    return out, at


def table(text):
    """{codepoint: reading}"""
    pool, pool_size = readings(text)
    cps = numbers('g_pinyin_codepoints', text)
    offsets = numbers('g_pinyin_offsets', text)
    check(len(cps) == len(offsets), 'the codepoint and offset arrays differ in length')
    count = int(re.search(r'#define PINYIN_CODEPOINTS (\d+)', text).group(1))
    check(count == len(cps), 'PINYIN_CODEPOINTS is not the array length')
    check(all(a < b for a, b in zip(cps, cps[1:])), 'the codepoints are not sorted')
    check(len({w for _, w in pool}) == len(pool), 'a reading is listed twice')
    by_offset = dict(pool)
    out = {}
    for cp, off in zip(cps, offsets):
        reading = by_offset.get(off)
        check(reading is not None, 'offset %d of U+%04x is not a reading start' % (off, cp))
        if reading is None:
            continue
        check(re.fullmatch(r'[a-z]+', reading) is not None, 'U+%04x reads %r: not plain a-z' % (cp, reading))
        check(off + len(reading) < pool_size, 'U+%04x reads past the pool' % cp)
        out[cp] = reading
    return out


def main():
    with open(TABLE, encoding='utf-8') as f:
        text = f.read()
    text = re.sub(r'//[^\n]*', '', text)  # (the arrays' own comments hold numbers: drop them first)
    got = table(text)
    print('%s: %d characters, %d readings' % (os.path.relpath(TABLE, ROOT), len(got),
                                              len(set(got.values()))))
    for ch, want in KNOWN.items():
        check(got.get(ord(ch)) == want, 'U+%04x reads %r, not %r' % (ord(ch), got.get(ord(ch)), want))
    check(all(cp <= 0xFFFF for cp in got), 'a codepoint above U+FFFF: not a uint16')
    if failures:
        for f in failures[:20]:
            print('FAIL', f)
        print('%d failure(s)' % len(failures))
        return 1
    print('ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
