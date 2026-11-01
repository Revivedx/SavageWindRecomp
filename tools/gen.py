"""Generate C for a whole NSO module.

Usage: gen.py <name> <nso> <layout.txt> <outdir> [--hle hle_symbols.txt] [--chunk N] [--simd asm|c]
       gen.py layout <exefs> <outdir>
       gen.py all <exefs> <outdir> [--hle hle_symbols.txt] [--simd asm|c]

The module's guest address comes from layout.txt, which `gen.py layout` writes (with layout.c) from the exefs
(tools/layout.py). `all` does that and then generates every module into <outdir>/<name>, with --hle for all of them
but main (`make gen`).

The main module has to be a game version Savage Wind supports (tools/game_sites.py VERSIONS): its sites are looked
up there, for tools/patches.py and for the runtime (layout.c). --any-version takes any main module whose sites the
patterns all find (bringing up a new version); --no-sites generates without sites or patches (the made-up modules
of tools/test_layout.py).

--simd: FP/SIMD/CRC32 as AArch64 inline asm (default on arm64 hosts) or portable C (default elsewhere).
"""
import argparse
import bisect
import faulthandler
import os
import platform
import re
import shutil
import struct
import sys
import time
from collections import Counter

import capstone
import unicorn as uc
from unicorn import arm64_const as A

import game_sites
import layout
import patches
from analyze import eh_frame_extents, eh_frame_functions
from lifter import Insn, Lifter, Unsupported, bits, emit_asm_batch, hx, sext
from nso import Nso, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE, header

SCRATCH_MEM = 0x7300000000


def sext26(w):
    v = w & 0x03FFFFFF
    return v - 0x04000000 if v & 0x02000000 else v


def fname(a):
    return f'f_{a:x}'


SETJMP_SYMS = ('setjmp', '_setjmp', '__setjmp')


def setjmp_call(t, ret):
    """A call to the guest setjmp. The recompiled code can't resume in the middle of a function, where the guest
    longjmp returns to, so the call gets a host setjmp in the C function of the caller (runtime/hle/setjmp.c):
    the guest longjmp (HLE) restores the registers the guest setjmp saved and jumps back here with x0 set."""
    return (f'{{ jmp_buf *hb_ = rt_setjmp_slot(c, c->x[0]); '
            f'if (!setjmp(*hb_)) {{ c->x[30] = {hx(ret)}; {fname(t)}(c); }} }}')


def sanitize(sym):
    return 'hle_' + re.sub(r'[^A-Za-z0-9_]', '_', sym)


class Module:
    def __init__(self, name, path, base, simd='asm'):
        self.name = name
        self.nso = Nso(path)
        self.base = base
        n = self.nso
        self.lo = base + n.text[0]
        self.hi = base + n.text[0] + n.text[1]
        self.words = struct.unpack(f'<{n.text[1] // 4}I', bytes(n.image[n.text[0]:n.text[0] + n.text[1]]))
        self.lifter = Lifter(base, simd)
        self.cache = {}
        self.stats = Counter()
        self.mu = None
        self.cs = None  # (capstone, made on first use: writes_gpr)
        # the functions as compiled: discovery may split one into several C functions, a jump table spans all of it
        self.fdes = sorted((base + a, base + b) for a, b in eh_frame_extents(n) if b > a)
        self.fde_starts = [a for a, _ in self.fdes]
        self.jt_cache = {}
        self.setjmps = self.setjmp_targets()
        self.sites, self.patches = {}, {}  # (main: tools/game_sites.py, tools/patches.py)

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def fde(self, a):
        """(start, end) of the eh_frame function holding a, or None (code without unwind info)."""
        i = bisect.bisect_right(self.fde_starts, a) - 1
        if i >= 0 and a < self.fdes[i][1]:
            return self.fdes[i]
        return None

    def word(self, a):
        return self.words[(a - self.lo) // 4]

    def setjmp_targets(self):
        """Addresses whose call is a call to setjmp: the function itself (a module defining it) and the PLT stubs
        bound to it (adrp x16; ldr x17, [x16, #o]; add x16, x16, #o; br x17 through a JUMP_SLOT slot of it)."""
        n, b = self.nso, self.base
        out = {b + v for s, info, shndx, v, size in n.syms if shndx and s in SETJMP_SYMS}
        slots = {b + off for off, typ, si, add in n.relocs
                 if typ == R_AARCH64_JUMP_SLOT and si < len(n.syms) and n.syms[si][0] in SETJMP_SYMS}
        if not slots:
            return out
        for a in range(self.lo, self.hi - 12, 4):
            w = self.word(a)
            if (w & 0x9F00001F) != 0x90000010 or self.word(a + 12) != 0xD61F0220:  # adrp x16 ... br x17
                continue
            ld = self.word(a + 4)
            if (ld & 0xFFC003FF) != 0xF9400211:  # ldr x17, [x16, #imm]
                continue
            page = sext(((w >> 29) & 3) | ((w >> 5) & 0x7FFFF) << 2, 21) << 12
            if (a & ~0xFFF) + page + ((ld >> 10) & 0xFFF) * 8 in slots:
                out.add(a)
        return out

    def lift(self, a):
        ins = self.cache.get(a)
        if ins is None:
            try:
                ins = self.lifter.lift(self.word(a), a)
            except Unsupported as e:
                self.stats['unsupported'] += 1
                ins = Insn('trap', f'rt_udf(c, {hx(a)}); /* {e} */')
            self.cache[a] = ins
        return ins

    def boundaries(self):
        n, b = self.nso, self.base
        starts = set()
        for a in eh_frame_functions(n):
            starts.add(b + a)
        for name, info, shndx, value, size in n.syms:
            if shndx and (info & 0xF) == 2:
                starts.add(b + value)
        for off, typ, sym, add in n.relocs:
            if typ == R_AARCH64_RELATIVE:
                starts.add(b + add)
        starts = sorted(a for a in starts if self.in_text(a))
        return starts

    # ------------------------------------------------------------ jump tables

    def emu(self):
        if self.mu is None:
            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
            n = self.nso
            mu.mem_map(self.base, len(n.image))
            mu.mem_write(self.base, bytes(n.image))
            mu.mem_map(SCRATCH_MEM, 0x100000)
            mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)

            # a load before the index is set (often the one that fetches the index) can go through a register
            # nothing set: it reads zeros instead of stopping the emulation
            def zero_page(mu, access, addr, size, value, data):
                try:
                    mu.mem_map(addr & ~0xFFF, 0x1000)
                except uc.UcError:
                    return False
                return True
            mu.hook_add(uc.UC_HOOK_MEM_READ_UNMAPPED | uc.UC_HOOK_MEM_WRITE_UNMAPPED, zero_page)
            self.mu = mu
        return self.mu

    def jump_table(self, br_pc, entry, end):
        """Resolve `br xN` targets from a bounds-checked jump table, or None. The table's base, its index
        math and its targets can be anywhere in the function the br belongs to (its eh_frame range), also
        outside the piece entry..end that discovery made of it; code without an FDE is just entry..end."""
        lo, hi = self.fde(br_pc) or (entry, end)
        key = (br_pc, lo, hi)
        if key not in self.jt_cache:  # (discovery scans a piece again each time it is split)
            r = self._jump_table_fallthrough(br_pc, lo, hi)
            if r is None:
                r = self._jump_table_taken(br_pc, lo, hi)
            self.jt_cache[key] = r
        return self.jt_cache[key]

    def _index_regs(self, start, at, idx):
        """The index register plus registers holding the same value in start..at: sources it was
        copied/masked from, and copies of those (the table lookup sometimes recomputes the index
        from another copy after the bounds check)."""
        def copy_src(w):
            if (w & 0x7F800000) == 0x12000000:  # AND (imm)
                return bits(w, 5, 5)
            if (w & 0x7FE0FFE0) == 0x2A0003E0:  # MOV (ORR with zr)
                return bits(w, 16, 5)
            if (w & 0x7F800000) in (0x53000000, 0x13000000):  # UBFM/SBFM (uxtb, uxth, sxtw...)
                return bits(w, 5, 5)
            return None
        regs = {idx}
        changed = True
        while changed:
            changed = False
            for a in range(start, at, 4):
                w = self.word(a)
                rd, src = w & 31, copy_src(w)
                if src is None or src >= 31 or rd >= 31:
                    continue
                for x, y in ((rd, src), (src, rd)):
                    if x in regs and y not in regs:
                        regs.add(y)
                        changed = True
        return sorted(regs)

    def _set_index(self, mu, regs, i):
        for r in regs:
            mu.reg_write(A.UC_ARM64_REG_X0 + r if r < 29 else A.UC_ARM64_REG_X29, i)

    def _sets_flags(self, a):
        ins = self.lift(a)
        if ins.kind == 'asm':
            return ins.asm.flags
        return ins.kind == 'op' and re.search(r'\b(adds|subs|adcs|sbcs|logic)\d+\(|nzcv', ins.code) is not None

    def _guard_cmp(self, guard, lo):
        """The `cmp idx, #imm` that sets the flags a bounds check at guard tests, or None. Only straight-line
        code without other flag writes may come in between (cbz/tbz included: the compiler interleaves them)."""
        a = guard - 4
        while a >= lo and guard - a <= 4 * 12:
            w = self.word(a)
            if (w & 0x7F80001F) == 0x7100001F:  # SUBS (imm) with Rd = zr, i.e. CMP #imm
                return None if bits(w, 22, 1) else a
            if self.lift(a).kind not in ('op', 'asm', 'bcond') or self._sets_flags(a):
                return None
            a -= 4
        return None

    def _emulate_table(self, br_pc, lo, hi, start, set_at, idx_regs, count):
        """Run start..br once per index value (set in idx_regs on reaching set_at) and collect the br
        targets, which must lie in lo..hi. The window is straight-line code where a conditional branch falls
        through (the in-bounds path). First replays the address materialization (ADRP/ADR/ADD-imm) from the
        function entry lo, so table bases computed before loops or branches are known."""
        mu = self.emu()
        rn = bits(self.word(br_pc), 5, 5)
        targets = []
        try:
            for r in range(29):
                mu.reg_write(A.UC_ARM64_REG_X0 + r, SCRATCH_MEM + 0x80000)
            mu.reg_write(A.UC_ARM64_REG_SP, SCRATCH_MEM + 0x80000)
            for a in range(lo, start, 4):
                w = self.word(a)
                if (w & 0x1F000000) == 0x10000000 or (w & 0xFF800000) == 0x91000000:
                    mu.emu_start(a, 0, count=1)
            ctx = mu.context_save()  # (the replay doesn't depend on the index: once per table)
            for i in range(count):
                mu.context_restore(ctx)
                for a in range(start, br_pc, 4):
                    if a == set_at:
                        self._set_index(mu, idx_regs, i)
                    k = self.lift(a).kind
                    if k == 'bcond':
                        continue
                    if k not in ('op', 'asm'):
                        return None
                    mu.emu_start(a, 0, count=1)  # (single steps: Unicorn's `until` is unreliable mid translation block)
                t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                                (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
                if not (lo <= t < hi) or t % 4:
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        if count > 1 and len(set(targets)) == 1:  # (the index set wasn't the one the table is read with)
            return None
        return sorted(set(targets))

    def _jump_table_taken(self, br_pc, lo, hi):
        """Form: cmp idx, #N ; b.ls/b.lo table ... table: <address math> ; br"""
        # start of the block holding the br
        s = br_pc
        while s - 4 >= lo and br_pc - (s - 4) <= 4 * 12 and self.lift(s - 4).kind in ('op', 'asm'):
            s -= 4
        for g in range(lo, hi, 4):
            w = self.word(g)
            if (w & 0xFF000010) != 0x54000000 or bits(w, 0, 4) not in (9, 3):
                continue
            t = g + sext(bits(w, 5, 19), 19) * 4
            if t != s:
                continue
            cmp = self._guard_cmp(g, lo)
            if cmp is None:
                continue
            limit = bits(self.word(cmp), 10, 12)
            count = limit + 1 if bits(w, 0, 4) == 9 else limit
            if count <= 0 or count > 4096:
                continue
            return self._emulate_table(br_pc, lo, hi, s, s, [bits(self.word(cmp), 5, 5)], count)
        return None

    def _jump_table_fallthrough(self, br_pc, lo, hi):
        """Form: cmp idx, #N ; b.hi/b.hs default ; <address math> ; br"""
        # find the guarding b.hi / b.hs, scanning backwards
        a = br_pc - 4
        guard = None
        while a >= lo and br_pc - a <= 4 * 32:
            w = self.word(a)
            if (w & 0xFF000010) == 0x54000000 and bits(w, 0, 4) in (8, 2):
                guard = a
                break
            if self.lift(a).kind not in ('op', 'asm', 'bcond'):
                return None
            a -= 4
        if guard is None:
            return None
        cmp = self._guard_cmp(guard, lo)
        if cmp is None:
            return None
        w = self.word(cmp)
        limit = bits(w, 10, 12)
        count = limit + 1 if bits(self.word(guard), 0, 4) == 8 else limit
        if count <= 0 or count > 4096:
            return None
        # window start: back from cmp to the first control-flow instruction
        start = cmp
        while start - 4 >= lo and cmp - (start - 4) <= 4 * 12 and self.lift(start - 4).kind in ('op', 'asm'):
            start -= 4
        return self._emulate_table(br_pc, lo, hi, start, cmp, self._index_regs(start, cmp, bits(w, 5, 5)), count)

    # ------------------------------------------------------------ functions

    def writes_gpr(self, a, rn):
        """True if the instruction at a writes general register rn (Xn or Wn). Its low 5 bits don't tell: they are
        also the register a store stores (`stp s9, s8, [sp, #0x80]`) and FP/SIMD destinations."""
        if self.cs is None:
            self.cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
            self.cs.detail = True
        ins = next(self.cs.disasm(struct.pack('<I', self.word(a)), a), None)
        if ins is None:
            return True  # (undecoded: stop there, as for a write)
        return any(ins.reg_name(r) in (f'x{rn}', f'w{rn}') for r in ins.regs_access()[1])

    def computed_target(self, br_pc, entry):
        """True if the br register comes from address arithmetic (a table) rather than a load."""
        rn = bits(self.word(br_pc), 5, 5)
        a = br_pc - 4
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if self.lift(a).kind not in ('op', 'asm'):
                return False
            if self.writes_gpr(a, rn):
                if (w & 0x1F000000) == 0x0B000000:  # ADD/SUB (shifted or extended register)
                    return True
                if (w & 0x9F000000) == 0x10000000:  # ADR
                    return True
                return False
            a -= 4
        return False

    def scan(self, entry, end):
        """Collect labels and external entries referenced by a function."""
        labels, ext, tables = {entry}, set(), {}
        terminated = False
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if ins.kind in ('b', 'bcond'):
                t = ins.target
                if entry <= t < end:
                    labels.add(t)
                elif self.in_text(t):
                    ext.add(t)
            elif ins.kind == 'bl':
                if self.in_text(ins.target):
                    ext.add(ins.target)
            elif ins.kind == 'br':
                jt = self.jump_table(a, entry, end)
                if jt:
                    tables[a] = jt
                    # (a target outside this piece of a split function is a boundary, reached by a tail call)
                    for t in jt:
                        (labels if entry <= t < end else ext).add(t)
                elif self.computed_target(a, (self.fde(a) or (entry,))[0]):
                    # unresolved table jump: allow jumping to any instruction of the function
                    tables[a] = 'all'
            terminated = ins.kind in ('b', 'br', 'ret', 'trap')
        if not terminated and end < self.hi:
            ext.add(end)
        return labels, ext, tables

    def render_single(self, ins, a, jump, refs):
        """C for one lifted instruction (no batching); used for patch variants."""
        k = ins.kind
        if k == 'asm':
            return [emit_asm_batch([ins.asm])]
        if k in ('op', 'trap'):
            return [ins.code]
        if k == 'b':
            return [jump(ins.target)]
        if k == 'bcond':
            return [f'if ({ins.cond}) {{ {jump(ins.target)} }}']
        if k == 'bl':
            t = ins.target
            if t in self.setjmps:
                refs.add(t)
                return [setjmp_call(t, a + 4)]
            if self.in_text(t):
                refs.add(t)
                return [f'c->x[30] = {hx(a + 4)}; {fname(t)}(c);']
            return [f'c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);']
        if k == 'blr':
            return [f'c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);']
        if k == 'br':
            return [f'c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);']
        if k == 'ret':
            return ['return;']
        raise Unsupported(k)

    def emit_function(self, entry, end, labels, tables, hle):
        if entry in hle:
            self._refs = set()
            return [f'void {fname(entry)}(Cpu *c) {{', f'    {sanitize(hle[entry])}(c);', '}']
        # Jump tables are emulated (jump_table), so their cases are known; one whose emulation missed some still
        # reaches the right instruction through the all-instruction fallback: a label on every instruction and a
        # switch over all of them. With an unresolved table ('all') that is the function's normal path. Otherwise
        # it lives in a cold copy of the function (<name>_r), entered on a miss only: in the function itself it
        # split every instruction into its own block and made LLVM's register coalescer take minutes per function.
        refs = set()
        if not tables or 'all' in tables.values():
            out = self._emit_body(entry, end, labels, tables, refs, 'plain')
        else:
            out = [f'static void {fname(entry)}_r(Cpu *c);']
            out += self._emit_body(entry, end, labels, tables, refs, 'fast')
            out += self._emit_body(entry, end, labels, tables, refs, 'resume')
        self._refs = refs
        return out

    def _emit_body(self, entry, end, labels, tables, refs, mode):
        """One C function for entry..end. mode: 'plain' (the all-instruction fallback inside when it has tables),
        'fast' (a table miss goes on in <name>_r) or 'resume' (<name>_r: from c->pc, through that fallback)."""
        if mode == 'resume':
            # (optnone: optimized, its switch made the copy as slow to build as the function used to be, and it is
            # never run in practice. Unoptimized, its frame can reach ~250 KB: guest threads have 256 MB host stacks)
            head = f'__attribute__((cold, noinline, optnone)) static void {fname(entry)}_r(Cpu *c) {{'
            out = [head, '    goto L_dispatch;']
        else:
            out = [f'void {fname(entry)}(Cpu *c) {{', f'    FN_ENTER({hx(entry)});']
        batch = []

        def flush():
            if batch:
                out.append('    ' + emit_asm_batch(batch))
                batch.clear()

        def jump(t):
            if entry <= t < end:
                return f'goto L_{t:x};'
            if not self.in_text(t):
                return f'c->pc = {hx(t)}; __attribute__((musttail)) return rt_dispatch(c);'
            refs.add(t)
            return f'__attribute__((musttail)) return {fname(t)}(c);'

        all_labels = mode == 'resume' or (mode == 'plain' and bool(tables))
        if all_labels:
            labels = set(range(entry, end, 4))
        miss = f'__attribute__((musttail)) return {fname(entry)}_r(c);' if mode == 'fast' else 'goto L_dispatch;'
        last = None
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if a in labels:
                flush()
                out.append(f'L_{a:x}:;')
            if a in self.patches:  # hand-written replacement for one instruction (tools/patches.py)
                flush()
                out.append('    ' + self.patches[a](self, a))
                last = 'op'
                continue
            k = ins.kind
            if k == 'asm':
                if len(batch) >= 24:
                    flush()
                batch.append(ins.asm)
                try:
                    emit_asm_batch(batch)
                except Unsupported:
                    batch.pop()
                    flush()
                    batch.append(ins.asm)
                last = k
                continue
            flush()
            if k == 'op' or k == 'trap':
                out.append('    ' + ins.code)
            elif k == 'b':
                out.append('    ' + jump(ins.target))
            elif k == 'bcond':
                out.append(f'    if ({ins.cond}) {{ {jump(ins.target)} }}')
            elif k == 'bl':
                t = ins.target
                if t in self.setjmps:
                    refs.add(t)
                    out.append('    ' + setjmp_call(t, a + 4))
                elif self.in_text(t):
                    refs.add(t)
                    out.append(f'    c->x[30] = {hx(a + 4)}; {fname(t)}(c);')
                else:
                    out.append(f'    c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);')
            elif k == 'blr':
                out.append(f'    c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);')
            elif k == 'br':
                jt = tables.get(a)
                if jt == 'all':
                    out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                    continue
                if jt:
                    cases = ' '.join(f'case {hx(t)}: {jump(t)}' for t in jt)
                    out.append(f'    switch ({ins.target}) {{ {cases} default: break; }}')
                    out.append(f'    c->pc = {ins.target}; {miss}')
                    continue
                out.append(f'    c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);')
            elif k == 'ret':
                out.append('    return;')
            last = k
        flush()
        if last not in ('b', 'br', 'ret', 'trap'):
            out.append('    ' + jump(end) if end < self.hi else f'    rt_udf(c, {hx(end)});')
        if all_labels:
            out.append('L_dispatch:')
            cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in range(entry, end, 4))
            out.append(f'    switch (c->pc) {{ {cases} default: break; }}')
            out.append('    __attribute__((musttail)) return rt_dispatch(c);')
        out.append('}')
        return out


def write_if_changed(path, text):
    """Leaves unchanged chunks untouched so make only recompiles what a patch affected."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w') as f:
        f.write(text)


def discover(m, starts):
    """Partition text into functions: address -> (end, labels, jump tables). Returns (entries, rounds)."""
    # Partition text into functions; any branch/call target outside its function becomes a new
    # boundary (splitting the containing function) until a fixpoint is reached.
    start_set = set(starts)
    for i, w in enumerate(m.words):
        if (w & 0xFC000000) == 0x94000000:
            t = m.lo + i * 4 + sext26(w) * 4
            if m.in_text(t):
                start_set.add(t)
    # Code addresses materialized in registers (callbacks, vtables built at runtime): ADR, or
    # ADRP followed shortly by ADD on the same register. One inside the function that takes it is a label
    # there instead, a jump table's base: as a boundary it would split the function away from its table.
    def own_label(pc, t):
        f = m.fde(pc)
        return f is not None and f[0] < t < f[1]

    adrp_regs = {}
    for i, w in enumerate(m.words):
        pc = m.lo + i * 4
        if (w & 0x9F000000) == 0x10000000:  # ADR
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            if m.in_text(pc + imm) and not own_label(pc, pc + imm):
                start_set.add(pc + imm)
        elif (w & 0x9F000000) == 0x90000000:  # ADRP
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            adrp_regs[w & 31] = ((pc & ~0xFFF) + (imm << 12), i)
        elif (w & 0xFFC00000) == 0x91000000:  # ADD (imm, 64-bit, no shift)
            rn, rd = (w >> 5) & 31, w & 31
            if rn in adrp_regs and i - adrp_regs[rn][1] <= 8:
                t = adrp_regs[rn][0] + ((w >> 10) & 0xFFF)
                if m.in_text(t) and not own_label(pc, t):
                    start_set.add(t)
    scanned = {}
    rounds = 0
    while True:
        rounds += 1
        order = sorted(start_set)
        new = set()
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            key = (a, end)
            if key not in scanned:
                scanned[key] = m.scan(a, end)
            new |= scanned[key][1] - start_set
        if not new:
            break
        start_set |= new
    entries = {}
    for i, a in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else m.hi
        labels, ext, tables = scanned[(a, end)]
        entries[a] = (end, labels, tables)
        # indirect branches of the final functions: jump tables resolved, all-labels fallback, plain dispatch
        for t in tables.values():
            m.stats['br_all_labels' if t == 'all' else 'jumptables'] += 1
        m.stats['br_dispatch'] += sum(m.lift(x).kind == 'br' for x in range(a, end, 4)) - len(tables)
    return entries, rounds


def main_sites(nso, base, opts):
    """(version, sites) of a main module (tools/game_sites.py); none with --no-sites"""
    if opts.no_sites:
        return '', {}
    return game_sites.for_module(nso, base, opts.any_version, lambda s: print(s, file=sys.stderr))


def generate(name, nso, base, outdir, opts, sites=None):
    t0 = time.time()
    m = Module(name, nso, base, opts.simd)
    if name == 'main':
        m.sites = sites if sites is not None else main_sites(m.nso, base, opts)[1]
        m.patches = patches.table(m.sites) if m.sites else {}
    starts = m.boundaries()
    hle = {}
    if opts.hle and os.path.exists(opts.hle):
        want = {l.strip() for l in open(opts.hle) if l.strip() and not l.startswith('#')}
        for sym, info, shndx, value, size in m.nso.syms:
            if shndx and sym in want:
                hle[base + value] = sym

    entries, rounds = discover(m, starts)
    print(f'{name}: {len(starts)} boundaries, {len(entries)} functions, {rounds} rounds, {dict(m.stats)}, '
          f'{time.time() - t0:.0f}s', file=sys.stderr)

    os.makedirs(outdir, exist_ok=True)
    order = sorted(entries)
    if opts.limit:
        order = order[:opts.limit]
    nchunks = 0
    ci = 0
    pending = None  # (address, lines, refs) of a big function that starts the next file
    while ci < len(order) or pending:
        body, refs, size, fns = [], set(), 0, []
        while (pending or ci < len(order)) and len(fns) < opts.chunk and size < opts.chunk_bytes:
            if pending:
                a, lines, r = pending
                pending = None
            else:
                a = order[ci]
                ci += 1
                end, labels, tables = entries[a]
                lines = m.emit_function(a, end, labels, tables, hle)
                r = getattr(m, '_refs', set())
                m._refs = set()
            n = sum(len(l) + 1 for l in lines)
            if n > opts.big_fn and fns:  # (a big function gets a file of its own: they compile in parallel)
                pending = (a, lines, r)
                break
            body += lines
            size += n
            fns.append(a)
            refs |= r
            if n > opts.big_fn:
                break
        path = os.path.join(outdir, f'{name}_{nchunks:03d}.c')
        text = ['#include "cpu.h"\n' + ('#include "simd.h"\n' if m.lifter.sc else '')]
        if any('rt_setjmp_slot(' in l for l in body):
            text += ['#include <setjmp.h>\njmp_buf *rt_setjmp_slot(Cpu *c, uint64_t gbuf);\n']
        text += [f'void {fname(r)}(Cpu *c);\n' for r in sorted(refs)]
        text += [f'void {sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & set(fns))]
        text += ['\n'.join(body), '\n']
        write_if_changed(path, ''.join(text))
        nchunks += 1
        # (the start screen's setup reads these for its progress bar: runtime/gamebuild.c)
        print(f'{name}: progress {ci}/{len(order)} files {nchunks}', file=sys.stderr, flush=True)
    for fn in os.listdir(outdir):  # an earlier run's extra chunks
        mm = re.fullmatch(re.escape(name) + r'_(\d{3})\.c', fn)
        if mm and int(mm.group(1)) >= nchunks:
            os.remove(os.path.join(outdir, fn))
    import io
    with io.StringIO() as f:
        f.write('#include "cpu.h"\n')
        for a in order:
            f.write(f'void {fname(a)}(Cpu *c);\n')
        # (a module without functions still gets its arrays: C has no empty initializer)
        f.write(f'const uint64_t {name}_fn_addrs[] = {{\n')
        f.write(',\n'.join(hx(a) for a in order) or '0')
        f.write('};\n')
        f.write(f'const GuestFn {name}_fn_ptrs[] = {{\n')
        f.write(',\n'.join(fname(a) for a in order) or '0')
        f.write('};\n')
        f.write(f'const uint64_t {name}_fn_count = {len(order)};\n')
        write_if_changed(os.path.join(outdir, f'{name}_table.c'), f.getvalue())
    print(f'{name}: wrote {nchunks} chunks, {time.time() - t0:.0f}s total', file=sys.stderr)


def main():
    # a crash in native code (Unicorn, capstone, Python itself) leaves the Python stack in the log instead of nothing
    # (the setup copies the end of this log into savagewind.log: runtime/gamebuild.c). Not on Windows: there it also
    # writes a "Windows fatal exception: access violation" stack for each fault Unicorn raises and handles itself (a
    # dozen or more in a run that works), which reads like the crash; a real one still shows as the exit status.
    if sys.platform != 'win32':
        try:  # (only the Python stack: 3.14's C stack, without symbols, would push it out of the lines the setup keeps)
            faulthandler.enable(**({'c_stack': False} if sys.version_info >= (3, 14) else {}))
        except (AttributeError, RuntimeError, ValueError, OSError):  # (no stderr to write to)
            pass
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument('name')  # a module (main, subsdk0..9, sdk), or layout / all
    ap.add_argument('input')  # its NSO; layout / all: the exefs folder
    ap.add_argument('paths', nargs='+')  # <layout.txt> <outdir>; layout / all: <outdir>
    ap.add_argument('--hle')
    ap.add_argument('--chunk', type=int, default=1500)  # functions per file at most
    ap.add_argument('--chunk-bytes', type=int, default=1500000)  # and about this much C (even compile times)
    ap.add_argument('--big-fn', type=int, default=100000)  # bytes of C from which a function gets its own file
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--any-version', action='store_true')  # (a main module of a version not supported yet)
    ap.add_argument('--no-sites', action='store_true')  # (made-up modules, tools/test_layout.py: no sites, no patches)
    ap.add_argument('--simd', choices=('asm', 'c'),
                    default='asm' if platform.machine().lower() in ('arm64', 'aarch64') else 'c')
    args = ap.parse_args()
    if len(args.paths) != (1 if args.name in ('layout', 'all') else 2):
        ap.error('wrong number of arguments')

    try:
        if args.name in ('layout', 'all'):
            exefs, outdir = args.input, args.paths[0]
            lay = layout.checked(exefs)
            layout.show(lay)
            main_base = next(b for n, b, *_ in lay if n == 'main')
            version, sites = main_sites(Nso(os.path.join(exefs, 'main')), main_base, args)
            print(f'  game version {version}, {len(sites)} sites')
            layout.write(lay, outdir, (version, sites))
            if args.name == 'all':
                present = {n for n, *_ in lay}
                for n in layout.LOAD_ORDER:  # a module gone from the exefs: its old C would still be built in
                    if n not in present and os.path.isdir(os.path.join(outdir, n)):
                        shutil.rmtree(os.path.join(outdir, n))
                hle = args.hle
                for n, base, size, build_id in lay:
                    args.hle = hle if n != 'main' else None
                    generate(n, os.path.join(exefs, n), base, os.path.join(outdir, n), args,
                             sites if n == 'main' else None)
            return
        lay = {n: (b, i) for n, b, s, i in layout.read(args.paths[0])}
        if args.name not in lay:
            raise layout.LayoutError(f'{args.paths[0]} has no module {args.name}')
        base, build_id = lay[args.name]
        if header(args.input)['build_id'] != build_id:
            raise layout.LayoutError(f'{args.input} is not the {args.name} that {args.paths[0]} was made from')
        generate(args.name, args.input, base, args.paths[1], args)
    except (layout.LayoutError, game_sites.SiteError) as e:
        sys.exit(f'gen.py: {e}')


if __name__ == '__main__':
    main()
