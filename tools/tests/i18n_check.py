#!/usr/bin/env python3
"""Menu translations (runtime/ui/i18n.mm): lists the texts each runtime/ui/i18n/<lang>.inc lacks.

The keys are the English texts themselves. This collects the string literals of the menu sources (adjacent literals
joined, as the compiler does) that read like text shown to the player - not IDs ("##..."), paths, environment variable
names or log messages - and compares them with each table's T("English", "...") entries.

An entry with an empty translation, T("...", ""), keeps the English text (names, units).

usage: tools/tests/i18n_check.py [lang ...]           report missing / unused entries (all tables when no lang given)
       tools/tests/i18n_check.py --missing <lang>     print the missing keys as T("...", "") lines
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCES = sorted(glob.glob(os.path.join(ROOT, 'runtime/ui/*.mm'))) + [
    os.path.join(ROOT, 'runtime/language.c'),
    os.path.join(ROOT, 'runtime/hle/bindings.c'),
]
SKIP_LINE = re.compile(r'\b(LOG|fprintf|printf|getenv|strcmp|strncmp|strstr|fopen|file_exists|mkdir|unlink|rt_resource|'
                       r'PushID|BeginChild|BeginTable|BeginPopup|OpenPopup|IsPopupOpen|InvisibleButton|GetID|Begin)\s*\(|'
                       r'#include|#define|SAVAGEWIND_|//')


def literals(text):
    """(line, string) for every string literal; adjacent literals (only whitespace / comments between) joined."""
    out, i, n, line = [], 0, len(text), 1
    pending, pend_line, last_end = None, 0, -1
    while i < n:
        c = text[i]
        if c == '\n':
            line += 1
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            line += text.count('\n', i, j)
            i = n if j < 0 else j + 2
            continue
        if c == "'":
            i += 3 if text[i + 1] != '\\' else 4
            continue
        if c == '"':
            j, buf = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == '\\':
                    e = text[j + 1]
                    buf.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'", '0': '\0'}.get(e, '\\' + e))
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            s = ''.join(buf)
            between = text[last_end:i] if pending is not None else ''
            if pending is not None and re.fullmatch(r'\s*', between):
                pending += s
            else:
                if pending is not None:
                    out.append((pend_line, pending))
                pending, pend_line = s, line
            last_end = j + 1
            i = j + 1
            continue
        i += 1
    if pending is not None:
        out.append((pend_line, pending))
    return out


def looks_like_text(s, in_tr=False):
    if not re.search(r'[A-Za-z]{2}', s) or s.startswith('##') or '###' in s:
        return False
    if in_tr and '/' not in s and '\\' not in s:  # (a word passed to tr() is text, however short)
        return True
    if re.fullmatch(r'[\w./\\:-]+\.(ttf|ttc|bfttf|pack|bin|txt|ini|png|dll|so|dylib|nso|npdm|mp4|webm)', s):
        return False
    if re.fullmatch(r'[a-z0-9_./-]+', s):  # identifiers, ini keys, folder names
        return False
    if re.fullmatch(r'[A-Z]{2}[a-z]{2}', s):  # region languages (USen)
        return False
    if s.startswith(('/', 'C:', 'file:')) or 'SAVAGEWIND' in s:
        return False
    return True


def keys():
    found = {}
    for path in SOURCES:
        text = open(path, encoding='utf-8').read()
        lines = text.split('\n')
        for ln, s in literals(text):
            src = lines[ln - 1] if ln - 1 < len(lines) else ''
            if SKIP_LINE.search(src.split('"')[0]) or not looks_like_text(s, 'tr(' in src):
                continue
            found.setdefault(s, f'{os.path.relpath(path, ROOT)}:{ln}')
    return found


def table(lang):
    path = os.path.join(ROOT, 'runtime/ui/i18n', lang + '.inc')
    if not os.path.exists(path):
        return {}
    text = open(path, encoding='utf-8').read()
    lits = literals(text)
    # T("en", "tr"): literals come in pairs, in order
    entries = {}
    for k in range(0, len(lits) - 1, 2):
        entries[lits[k][1]] = lits[k + 1][1]
    return entries


def c_quote(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t') + '"'


def main():
    want = keys()
    args = sys.argv[1:]
    if args[:1] == ['--missing']:
        have = table(args[1])
        for k, where in want.items():
            if k not in have:
                print(f'T({c_quote(k)}, "")  // {where}')
        return
    langs = args or sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(ROOT, 'runtime/ui/i18n/*.inc')))
    print(f'{len(want)} menu texts')
    bad = False
    for lang in langs:
        have = table(lang)
        missing = [k for k in want if k not in have]
        unused = [k for k in have if k not in want]
        print(f'{lang}: {len(have)} entries, {len(missing)} missing, {len(unused)} unused')
        for k in missing[:20]:
            print(f'  missing: {k[:90]!r}  ({want[k]})')
        for k in unused[:20]:
            print(f'  unused:  {k[:90]!r}')
        bad |= bool(missing)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
