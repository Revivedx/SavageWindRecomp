"""Function-local general registers for tools/gen.py (gen.py --locals).

Ported from TotK Recomp's recomp/regs.py (https://github.com/valentderah/totk-pc-recomp, commit f469c5a),
Copyright (c) 2026 Valiantsin Dzerakh, MIT License: third_party/totk-pc-recomp/LICENSE.

The lifter reads and writes the guest's X0-X30 and SP in the Cpu (c->x[n], c->sp), so every guest instruction loads
and stores memory, and clang cannot keep a register in a host register across a guest store: guest stores go through
pointers that may alias the Cpu. A generated function instead copies the registers it uses most into locals at its
entry (xN, sp_), works on those, and syncs with the Cpu only where something else reads or writes them: before a
call, an SVC, a trap, a patch site, a return or a tail exit it stores the registers it changed since the last sync
(spill), and after an SVC or a patch site it loads them all again (reload). The other registers stay in the Cpu.

After a call it reloads only X0-X18 and X30: AAPCS64 has the callee preserve X19-X29 and SP, so their locals still
hold what the Cpu holds. In Breath of the Wild what could break that is checked in docs/RECOMP-LOCALS.md: the only
runtime code that writes X19-X29 of a running thread's Cpu is the HLE longjmp (runtime/hle/setjmp.c), which never
returns to its caller, and the function it lands in reloads every register after its setjmp (gen.py setjmp_call).
Runtime code that saves and restores the whole Cpu around a guest call (runtime/pc_cursor.c, runtime/hle/nvn_boot.c)
puts back what it found. gen.py --no-callee-saved-trust reloads those registers too.

Only plain assignments write registers in the lifted C (`c->x[n] = ...`, `c->sp = ...`), and no lifted code takes a
register's address, so the rewrite is textual. Code that must see the Cpu itself (a patch site's hand-written C,
which calls into the runtime with c) is wrapped in OPAQUE ... END: its register accesses stay on the Cpu, and gen.py
spills before it and reloads after it. A register counts as dirty from a write until the next spill on some path:
plan() runs that forward over the function's control flow, so a call spills only what changed since the previous call
or the entry. Spilling a register that is not dirty is harmless (the local equals the Cpu), so where a case is
unclear the plan errs on spilling more.
"""
import re
from collections import Counter

REG = re.compile(r'c->x\[(\d+)\]|c->sp\b')
# (compound assignments too: hand-written C has `c->sp -= 0x30`; the lifter itself only writes `=`)
WRITE = re.compile(r'c->(?:x\[(\d+)\]|(sp))\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|(?:\+\+|--)\s*c->(?:x\[(\d+)\]|(sp))\b'
                   r'|c->(?:x\[(\d+)\]|(sp))\s*(?:\+\+|--)')
# Lifted instructions that hand the Cpu to the runtime: SVCs return, BRK and undefined words do not.
ESCAPE = re.compile(r'\brt_(?:svc|brk|udf)\(c\b')
SP = 31   # index of SP in the register sets
# Markers gen.py puts where the Cpu must be current, resolved by Plan.resolve(): spill before; reload all, or reload
# what a call may change, after. OPAQUE..END: text whose registers stay on the Cpu.
SPILL, RELOAD, RELOAD_CALL, OPAQUE, END = '\x00S', '\x00R', '\x00C', '\x00O', '\x00E'
CALLEE_SAVED = frozenset(range(19, 30)) | {SP}
# Locals per function: the most used registers. More cost more than they save, as clang then spills them to the stack
# and the syncs stay (TotK Recomp measured 300 hot functions: fewest instructions and memory operands at 4-6).
LOCALS = 6


def name(r):
    return 'sp_' if r == SP else f'x{r}'


def cpu(r):
    return 'c->sp' if r == SP else f'c->x[{r}]'


def visible(text):
    """text without its OPAQUE..END parts."""
    return re.sub('\x00O.*?\x00E', '', text, flags=re.S)


def refs(text):
    """Registers text reads or writes (outside opaque parts), once per occurrence."""
    return [SP if m.group(0) == 'c->sp' else int(m.group(1)) for m in REG.finditer(visible(text))]


def written(text):
    out = set()
    for m in WRITE.finditer(visible(text)):
        g = m.groups()
        for k in (0, 2, 4):
            if g[k] is not None:
                out.add(int(g[k]))
            if g[k + 1] is not None:
                out.add(SP)
    return out


class Plan:
    def __init__(self, regs, spill, kept):
        self.regs = regs     # sorted registers the function keeps in locals
        self.local = set(regs)
        self.spill = spill   # per instruction: registers to store at its sync points
        self.kept = kept     # registers a call leaves as they were (CALLEE_SAVED, or none)

    def decl(self):
        """Locals at the top of the function, loaded from the Cpu."""
        return 'uint64_t ' + ', '.join(f'{name(r)} = {cpu(r)}' for r in self.regs) + ';' if self.regs else ''

    def resolve(self, i, text):
        """text of instruction i with its registers on the locals and its sync markers expanded."""
        def local(m):
            r = SP if m.group(0) == 'c->sp' else int(m.group(1))
            return name(r) if r in self.local else m.group(0)

        parts = re.split('(\x00O.*?\x00E)', text, flags=re.S)
        text = ''.join(p[2:-2] if p.startswith(OPAQUE) else REG.sub(local, p) for p in parts)
        spill = ''.join(f'{cpu(r)} = {name(r)}; ' for r in sorted(self.spill[i]))
        reload = ''.join(f' {name(r)} = {cpu(r)};' for r in self.regs)
        reload_call = ''.join(f' {name(r)} = {cpu(r)};' for r in self.regs if r not in self.kept)
        return text.replace(SPILL, spill).replace(RELOAD, reload).replace(RELOAD_CALL, reload_call)


def strip(text):
    """text with the markers removed and opaque parts unwrapped: what the function is without locals."""
    for m in (SPILL, RELOAD, RELOAD_CALL, OPAQUE, END):
        text = text.replace(m, '')
    return text


def plan(texts, succ, clean, n_locals=LOCALS, kept=CALLEE_SAVED):
    """texts[i]: instruction i's C with markers; succ[i]: successor indices inside the function; clean: instructions
    after which every local equals the Cpu (a call, an SVC or a patch site, which spill and reload on every path
    through them); n_locals: how many registers to keep in locals; kept: registers a call does not reload."""
    n = len(texts)
    uses = Counter(r for t in texts for r in refs(t))
    regs = {r for r, _ in sorted(uses.items(), key=lambda kv: (-kv[1], kv[0]))[:n_locals]}
    writes = [written(t) & regs for t in texts]
    dirty = [set() for _ in range(n)]
    seen = [False] * n
    work = [0] if n else []
    if n:
        seen[0] = True
    while work:
        i = work.pop()
        out = set() if i in clean else dirty[i] | writes[i]
        for s in succ[i]:
            if not seen[s] or not out <= dirty[s]:
                seen[s] = True
                dirty[s] |= out
                work.append(s)
    every = set().union(*writes) if writes else set()
    for i in range(n):
        if not seen[i]:   # not reached from the entry in the plan's flow: anything may be dirty
            dirty[i] = every
    # A sync point stores what was dirty on entry to its instruction and what the instruction wrote before it
    # (c->x[30] of a call): all the instruction's writes, which is at most a little more.
    return Plan(sorted(regs), [dirty[i] | writes[i] if SPILL in texts[i] else set() for i in range(n)], kept)
