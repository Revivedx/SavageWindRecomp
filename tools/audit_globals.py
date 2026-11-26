#!/usr/bin/env python3
"""Which global variables does a function (and everything it calls) read and write?

Compiles a C file to LLVM IR (-O0, so every function and static local stays separate) and walks the call graph.
It answers "how entangled is this code with the rest of the file", for example before moving part of the render
thread's draw path to another thread (docs/PERF-RENDER-THREAD-SPLIT.md).

    tools/audit_globals.py [--cc clang] [--summary] FILE.c ROOT [ROOT...] -- <compile flags>
    tools/audit_globals.py --self-test

Example (one line): tools/audit_globals.py --summary runtime/nvn/vulkan/exec.c draw_ -- -Ithird_party/vma -Iruntime
-Iruntime/nvn -Iruntime/nvn/vulkan -I<SDL3 include dir>

Per root it prints the functions reached, the globals loaded and stored (static locals count as globals), the globals
whose address is passed to a call (the callee may read or write them), the callees outside the file and the indirect
calls (not followed). The closure follows every call, including cold ones (error paths, debug logging), so it is an
upper bound; the "self" numbers (the root's own body) are a floor. Reads and writes are found per instruction through
getelementptr chains of one function, not through pointers handed to other functions: those are the "passed" set.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

PTR = r'(@[\w.$\-]+|%[\w.$\-]+)'
CONST_RE = re.compile(r'\bconstant\b')


CGEP_RE = re.compile(r'getelementptr [\w ]*\([^()@]*?ptr (@[\w.$\-]+)[^()]*\)')


def flatten_const_gep(line):
    """Replace constant-expression GEPs on a global ("ptr getelementptr inbounds (%S, ptr @g, i32 0, i32 1)", which is
    how -O0 spells g.field and g[2]) by the bare global, so the access is attributed to @g."""
    prev = None
    while prev != line:
        prev, line = line, CGEP_RE.sub(r'\1', line)
    return line


def parse(ir_text):
    lines = [flatten_const_gep(l) for l in ir_text.split('\n')]
    consts = set()
    for l in lines:
        m = re.match(r'(@[\w.$\-]+) = (.*)', l)
        if m and CONST_RE.search(m.group(2)[:100]):
            consts.add(m.group(1))
    funcs, cur = {}, None
    for l in lines:
        m = re.match(r'define .*?(@[\w.$\-]+)\(', l)
        if m:
            cur = m.group(1)
            funcs[cur] = {'r': set(), 'w': set(), 'p': set(), 'calls': set(), 'ind': 0, 'map': {}}
            continue
        if l.startswith('}'):
            cur = None
            continue
        if cur is None:
            continue
        f, pm = funcs[cur], funcs[cur]['map']

        def base(p):
            return p if p.startswith('@') else pm.get(p)

        def live(b):
            return b and b not in consts

        m = re.match(r'\s*(%[\w.$\-]+) = getelementptr[^,]*, ptr ' + PTR, l)
        if m:
            b = base(m.group(2))
            if b:
                pm[m.group(1)] = b
            continue
        m = re.match(r'\s*(%[\w.$\-]+) = load [^,]*, ptr ' + PTR, l)
        if m:
            b = base(m.group(2))
            if live(b):
                f['r'].add(b)
            continue
        m = re.match(r'\s*store (?:atomic )?(?:volatile )?[^,]*, ptr ' + PTR, l)
        if m:
            b = base(m.group(1))
            if live(b):
                f['w'].add(b)
            continue
        m = re.match(r'\s*(?:%[\w.$\-]+ = )?(?:atomicrmw|cmpxchg) [^,]*?ptr ' + PTR, l)
        if m:
            b = base(m.group(1))
            if live(b):
                f['r'].add(b)
                f['w'].add(b)
            continue
        m = re.match(r'\s*(?:%[\w.$\-]+ = )?(?:tail )?call [^@]*?' + PTR + r'\((.*)\)', l)
        if m:
            callee, args = m.group(1), m.group(2)
            ps = re.findall(PTR, args)
            if callee.startswith('%'):
                f['ind'] += 1
            elif callee.startswith(('@llvm.memcpy', '@llvm.memmove')):
                if len(ps) >= 2:
                    d, s = base(ps[0]), base(ps[1])
                    if live(d):
                        f['w'].add(d)
                    if live(s):
                        f['r'].add(s)
            elif callee.startswith('@llvm.memset'):
                if ps and live(base(ps[0])):
                    f['w'].add(base(ps[0]))
            elif not callee.startswith('@llvm.'):
                f['calls'].add(callee)
                for a in ps:
                    b = base(a)
                    if live(b):
                        f['p'].add(b)
    return funcs


def closure(funcs, root):
    seen, stack = set(), [root]
    while stack:
        x = stack.pop()
        if x in seen or x not in funcs:
            continue
        seen.add(x)
        stack.extend(funcs[x]['calls'])
    r, w, p, ext, ind = set(), set(), set(), set(), 0
    for x in seen:
        r |= funcs[x]['r']
        w |= funcs[x]['w']
        p |= funcs[x]['p']
        ind += funcs[x]['ind']
        ext |= {c for c in funcs[x]['calls'] if c not in funcs}
    return seen, r, w, p, ext, ind


def compile_ir(cc, src, flags):
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / 'a.ll'
        cmd = [cc, '-O0', '-fno-discard-value-names', '-S', '-emit-llvm', '-w', *flags, str(src), '-o', str(out)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
        return out.read_text()


def report(funcs, roots, summary):
    for root in roots:
        r0 = '@' + root
        if r0 not in funcs:
            print('%s: not in the file' % root)
            continue
        seen, r, w, p, ext, ind = closure(funcs, r0)
        f = funcs[r0]
        print('%-30s %3d functions, loads %3d, stores %3d, address passed %2d, outside callees %3d, indirect %d'
              ' | own body: loads %2d, stores %2d, calls %2d' %
              (root, len(seen), len(r), len(w), len(p), len(ext), ind, len(f['r']), len(f['w']), len(f['calls'])))
        if not summary:
            print('   stored:', ' '.join(sorted(g[1:] for g in w)))
            print('   loaded only:', ' '.join(sorted(g[1:] for g in r - w)))
            print('   address passed only:', ' '.join(sorted(g[1:] for g in p - r - w)))
            print('   outside:', ' '.join(sorted(e[1:] for e in ext)))


SELF_TEST = r'''
int g_a, g_b, g_c, g_arr[4];
struct S { int x, y; } g_s;
static int hidden;
const int k_const = 7;
static void leaf(int *p) { *p = 1; }
static int reader(void) { return g_a + k_const; }
static void writer(void) { g_b = 2; g_s.y = 3; g_arr[2] = 4; }
static int with_static(void) { static int n; return ++n; }
void cold(void) { g_c = 1; }
void root(int v) {
    if (v) writer(); else cold();
    hidden = reader() + with_static();
    leaf(&g_a);
}
void lone(void) { g_b++; }
'''


def self_test(cc):
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / 't.c'
        src.write_text(SELF_TEST)
        funcs = parse(compile_ir(cc, src, []))
    names = lambda s: sorted(g[1:] for g in s)
    seen, r, w, p, ext, ind = closure(funcs, '@root')
    ok = True

    def check(what, got, want):
        nonlocal ok
        if got != want:
            ok = False
            print('FAIL %s: got %s, want %s' % (what, got, want))

    check('functions', sorted(x[1:] for x in seen), ['cold', 'leaf', 'reader', 'root', 'with_static', 'writer'])
    check('stores include writer and cold', [g for g in names(w) if g in ('g_b', 'g_c', 'g_s', 'g_arr', 'hidden')],
          ['g_arr', 'g_b', 'g_c', 'g_s', 'hidden'])
    check('static local is a global', 'with_static.n' in names(w), True)
    check('const not counted', 'k_const' in names(r | w), False)
    check('read', 'g_a' in names(r), True)
    check('address passed', names(p), ['g_a'])
    check('closure of lone', names(closure(funcs, '@lone')[2]), ['g_b'])
    print('self-test', 'ok' if ok else 'FAILED')
    return 0 if ok else 1


def main():
    argv = sys.argv[1:]
    cc, summary = 'clang', False
    if '--cc' in argv:
        i = argv.index('--cc')
        cc = argv[i + 1]
        del argv[i:i + 2]
    if '--self-test' in argv:
        return self_test(cc)
    if '--summary' in argv:
        argv.remove('--summary')
        summary = True
    if '--' in argv:
        i = argv.index('--')
        args, flags = argv[:i], argv[i + 1:]
    else:
        args, flags = argv, []
    if len(args) < 2:
        print(__doc__)
        return 2
    report(parse(compile_ir(cc, args[0], flags)), args[1:], summary)
    return 0


if __name__ == '__main__':
    sys.exit(main())
