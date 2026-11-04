"""Function-local NZCV for tools/gen.py (gen.py --locals).

Ported from TotK Recomp's recomp/flags.py (https://github.com/valentderah/totk-pc-recomp, commit f469c5a),
Copyright (c) 2026 Valiantsin Dzerakh, MIT License: third_party/totk-pc-recomp/LICENSE.

The lifter writes flags the way the guest does: every flag-setting instruction packs N, Z, C and V into c->nzcv
(subs64() and friends in runtime/cpu.h) and every reader unpacks them (COND_xx(c)). Compilers keep NZCV inside a
function, and AAPCS64 neither passes it to a callee nor preserves it across a call. So a generated function keeps its
flags in a local `Flags F` (runtime/cpu.h), which clang holds in registers, and writes c->nzcv only where it leaves
for another function other than by a call or a return, and before a patch site (see plan()).

A flag-setting instruction of a known kind (subtract, add or logic result; 32 or 64 bits) stores its operands in F.a
and F.b. A reader all of whose reaching definitions are of one kind computes its condition from those operands, so
`cmp x1, x2; b.lt` becomes a signed compare in C. Instructions whose result is only known packed (CCMP, FCMP, MSR
NZCV, ADCS) write F.nz, the packed form; a reader whose definitions are of several kinds reads F.nz, and those
definitions then also write it.

Unlike TotK Recomp's, a read that can see the flags a callee left (or a patch site's hand-written C) reads them: the
call counts as a definition held in c->nzcv, which the function then loads into F.nz right after it (Plan.load).
AAPCS64 code never reads a callee's flags, so such a read only sits on a path that does not run (code after a call to
a noreturn function), and the load costs nothing elsewhere; it keeps what the function does the same as without
locals there too. Plan.after_call counts them.
"""
import re

KIND_DEF = re.compile(r'\b(subs|adds|logic)(32|64)\(c, ')
CARRY_DEF = re.compile(r'\b(adcs|sbcs)(32|64)\(c, ')
CARRY_USE = re.compile(r'\b(adc|sbc)(32|64)\(c, ')
COND = re.compile(r'COND_([A-Z]{2})\(c\)')
NZ_DEF = re.compile(r'c->nzcv\s*=(?!=)')
NZ_USE = re.compile(r'c->nzcv(?!\s*=(?!=))')   # (MRS: `c->x[n] = c->nzcv;`)
KIND_NAME = {'subs': 'sub', 'adds': 'add', 'logic': 'logic'}

ENTRY = 'entry'
MEM = 'mem'   # kind of a definition the function finds in c->nzcv: a call's, a patch site's


class FlagError(Exception):
    pass


def effect(code):
    """(reads flags, kind of the flags written or None) of one instruction's C."""
    reads = bool(COND.search(code) or CARRY_USE.search(code) or CARRY_DEF.search(code) or NZ_USE.search(code))
    if NZ_DEF.search(code) or CARRY_DEF.search(code):
        return reads, 'nz'
    defs = KIND_DEF.findall(code)
    if len(defs) > 1:
        raise FlagError(f'several flag definitions in one instruction: {code}')
    if defs:
        op, size = defs[0]
        return reads, KIND_NAME[op] + size
    return reads, None


class Plan:
    """Per instruction: the kind its flag reads use, whether its flag definition also writes F.nz, what it writes
    to c->nzcv before it leaves for another function, and whether F.nz takes c->nzcv after it (a call's flags)."""

    def __init__(self, n):
        self.reader = [None] * n
        self.pack = [False] * n
        self.exit = [None] * n      # kind of the flags to publish in c->nzcv at an exit or a patch site, or None
        self.load = [False] * n     # F.nz = c->nzcv after this call or patch site
        self.entry = False          # F.nz starts as c->nzcv: flags from the entry reach a read or an exit
        self.entry_read = False     # they reach a read, not only an exit
        self.after_call = 0         # reads that can see flags a callee left (see the module docstring)
        self.used = False

    def decl(self):
        """Declaration of F at the top of the function."""
        return 'Flags F = {.nz = c->nzcv};' if self.entry else 'Flags F = {0};'

    def publish(self, i):
        """Statement writing the flags to c->nzcv at instruction i's exit ('' when it need not)."""
        k = self.exit[i]
        return f'c->nzcv = fnz_{k}(&F); ' if k else ''

    def reload(self, i):
        """Statement loading F.nz from c->nzcv after call or patch site i ('' when nothing reads it)."""
        return ' F.nz = c->nzcv;' if self.load[i] else ''

    def rewrite(self, i, code):
        """code of instruction i with its flag reads and writes on F."""
        rk = self.reader[i]
        _, dk = effect(code)
        if rk:
            code = COND.sub(lambda m: f'fc_{rk}(&F, CC_{m.group(1)})', code)
            code = CARRY_DEF.sub(lambda m: f'{m.group(1)}{m.group(2)}f(&F, fc_{rk}(&F, CC_CS), ', code)
            code = CARRY_USE.sub(lambda m: f'{m.group(1)}{m.group(2)}f(fc_{rk}(&F, CC_CS), ', code)
            code = NZ_USE.sub(f'fnz_{rk}(&F)', code)
        if dk == 'nz':
            code = NZ_DEF.sub('F.nz =', code)
            code = re.sub(r'\(void\)(subs|adds)(32|64)\(c, ', lambda m: f'F.nz = nz_{KIND_NAME[m.group(1)]}{m.group(2)}(',
                          code)
        elif dk:
            code = KIND_DEF.sub(lambda m: f'f{KIND_NAME[m.group(1)]}{m.group(2)}(&F, ', code)
            if self.pack[i]:
                code += f' F.nz = fnz_{dk}(&F);'
        if 'c->nzcv' in code or COND.search(code) or KIND_DEF.search(code) or CARRY_USE.search(code):
            raise FlagError(f'flag access left after the rewrite: {code}')
        return code


def plan(effects, succ, mems, exits):
    """effects[i]: (reads, kind or None); succ[i]: successor indices inside the function; mems: calls and patch
    sites, whose flags afterwards are in c->nzcv; exits: indices that can leave for another function other than by a
    call or a return (a tail branch, falling into the next function, a br through rt_dispatch), and patch sites,
    whose C reads c->nzcv.

    Discovery can start a function inside another one (a branch target, a materialised address), and the code
    before it then falls or branches into it with live flags. So an exit publishes the flags in c->nzcv, and a
    function whose reads can see its entry flags starts F.nz from c->nzcv. Calls and returns publish nothing: AAPCS64
    code neither passes flags to a callee nor reads a callee's."""
    n = len(effects)
    p = Plan(n)
    if not any(r or k for r, k in effects):
        return p
    p.used = True
    reach = [set() for _ in range(n)]
    reach[0].add(ENTRY)
    work = [0]
    while work:
        i = work.pop()
        out = {i} if effects[i][1] or i in mems else reach[i]
        for s in succ[i]:
            if not out <= reach[s]:
                reach[s] |= out
                work.append(s)

    def kind(d):
        return 'nz' if d == ENTRY or d in mems else effects[d][1]

    def use(r):
        """Kind a use of the definitions r reads; several kinds read the packed form, which they then all write."""
        for d in r:
            if d == ENTRY:
                p.entry = True
            elif d in mems:
                p.load[d] = True
        kinds = {kind(d) for d in r}
        if len(kinds) == 1:
            return kinds.pop()
        for d in r:
            if d != ENTRY and d not in mems:
                p.pack[d] = True
        return 'nz'   # several kinds, or none (unreachable)

    for i, (reads, _) in enumerate(effects):
        if reads:
            p.after_call += any(d in mems for d in reach[i])
            p.reader[i] = use(reach[i])
            p.entry_read |= ENTRY in reach[i]
    for i in exits:
        # Flags from the entry, a call or a patch site alone are still in c->nzcv.
        r = reach[i]
        if any(d != ENTRY and d not in mems for d in r):
            p.exit[i] = use(r)
    return p
