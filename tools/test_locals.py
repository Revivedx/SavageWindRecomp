"""Differential test of gen.py --locals (tools/regs.py, tools/flags.py): whole generated functions against Unicorn.

No NSO needed. Makes random hand-assembled functions: integer arithmetic, compares and their readers (b.cond, csel,
ccmp, adc, mrs nzcv), stack loads and stores, forward branches and a counted loop, and the places where the Cpu must
be current in the middle of a function:
- calls (bl to an AAPCS function that saves and restores x19/x20, blr through rt_dispatch);
- SVCs (the harness's rt_svc reads x19, x20, x28 and writes x0, x1, x7 and x21);
- patch sites (tools/patches.py form: hand-written C that calls the runtime with c, which reads x3, x19, x4 and NZCV and
  writes x0, x9 and NZCV, then `goto`s a label of the function; the C itself does `c->x[21] += 5`);
- setjmp/longjmp as Breath of the Wild's are recompiled (gen.py setjmp_call, an HLE longjmp that restores the
  registers the guest setjmp saved and longjmps into the C function of the setjmp's caller);
- tail exits (b to another function, b.cond to another function, br through rt_dispatch), to code that reads the
  flags it is entered with and to code that sets them first.
Unicorn runs the guest code itself (the SVC words, the patch site's word and the longjmp are emulated by a hook);
the module is generated through gen.py's Module.emit_function with --locals and without, compiled and run on the
same states, and X0-X30, SP and the stack memory must match Unicorn's in both (NZCV is seen through the exit
targets, which copy it into x16 / x17).

Usage: test_locals.py [--functions N] [--cases K] [--seed S] [--no-callee-saved-trust] [--reg-locals N]
"""
import argparse
import os
import platform
import random
import struct
import subprocess
import sys
import zlib
from collections import Counter

import unicorn as uc
from unicorn import arm64_const as A

import gen
from lifter import Lifter, hx

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CODE, CODE_SIZE = 0x7300000000, 0x100000
DATA, DATA_SIZE = 0x7310000000, 0x10000
SP0 = DATA + 0x8000
STOP = CODE + CODE_SIZE - 0x1000
FRAME = 0x100     # each test function's stack frame: slots at [sp, #0..0x78], jmp_buf at [sp, #0x80]
SVC_STUB, SVC_PATCH = 0x7f, 0x7e
POOL = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 19, 20, 21, 22, 23, 24, 25, 28]   # registers random instructions use
COUNTER = 11      # the loop's counter (random instructions leave it alone)
M64 = (1 << 64) - 1


# ---------------------------------------------------------------- hand assembly
def addsub_imm(sf, op, s, rd, rn, imm): return 0x11000000 | sf << 31 | op << 30 | s << 29 | imm << 10 | rn << 5 | rd
def addsub_reg(sf, op, s, rd, rn, rm, sh=0, amt=0):
    return 0x0B000000 | sf << 31 | op << 30 | s << 29 | sh << 22 | rm << 16 | amt << 10 | rn << 5 | rd
def logic_reg(sf, opc, rd, rn, rm, sh=0, amt=0, n=0):
    return 0x0A000000 | sf << 31 | opc << 29 | sh << 22 | n << 21 | rm << 16 | amt << 10 | rn << 5 | rd
def csel(sf, op, o2, rd, rn, rm, cond): return 0x1A800000 | sf << 31 | op << 30 | rm << 16 | cond << 12 | o2 << 10 | rn << 5 | rd
def ccmp_imm(sf, op, rn, imm, cond, nzcv): return 0x3A400800 | sf << 31 | op << 30 | imm << 16 | cond << 12 | rn << 5 | nzcv
def ccmp_reg(sf, op, rn, rm, cond, nzcv): return 0x3A400000 | sf << 31 | op << 30 | rm << 16 | cond << 12 | rn << 5 | nzcv
def adc(sf, op, s, rd, rn, rm): return 0x1A000000 | sf << 31 | op << 30 | s << 29 | rm << 16 | rn << 5 | rd
def movz(sf, rd, imm, hw=0): return 0x52800000 | sf << 31 | hw << 21 | imm << 5 | rd
def mrs_nzcv(rt): return 0xD53B4200 | rt
def msr_nzcv(rt): return 0xD51B4200 | rt
def bcond(pc, to, cond): return 0x54000000 | (((to - pc) >> 2) & 0x7FFFF) << 5 | cond
def cbz(sf, op, rt, pc, to): return 0x34000000 | sf << 31 | op << 24 | (((to - pc) >> 2) & 0x7FFFF) << 5 | rt
def b(pc, to): return 0x14000000 | (((to - pc) >> 2) & 0x3FFFFFF)
def bl(pc, to): return 0x94000000 | (((to - pc) >> 2) & 0x3FFFFFF)
def ldr(rt, rn, off): return 0xF9400000 | (off // 8) << 10 | rn << 5 | rt
def str_(rt, rn, off): return 0xF9000000 | (off // 8) << 10 | rn << 5 | rt
def stp_pre(rt, rt2, rn, off): return 0xA9800000 | ((off // 8) & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt
def ldp_post(rt, rt2, rn, off): return 0xA8C00000 | ((off // 8) & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt
def stp(rt, rt2, rn, off): return 0xA9000000 | ((off // 8) & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt
def ldp(rt, rt2, rn, off): return 0xA9400000 | ((off // 8) & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt
def adr(rd, pc, to): return 0x10000000 | ((to - pc) & 3) << 29 | (((to - pc) >> 2) & 0x7FFFF) << 5 | rd
def svc(imm): return 0xD4000001 | imm << 5
def blr(rn): return 0xD63F0000 | rn << 5
def br(rn): return 0xD61F0000 | rn << 5
RET = 0xD65F03C0


class Asm:
    """Words at consecutive addresses from `at`, with labels resolved at the end."""

    def __init__(self, at):
        self.at, self.words, self.fix, self.labels = at, [], [], {}

    def pc(self):
        return self.at + 4 * len(self.words)

    def put(self, w):
        self.words.append(w)

    def ref(self, f, label):   # f(pc, target) -> word, label resolved later
        self.fix.append((len(self.words), f, label))
        self.words.append(0)

    def label(self, name):
        self.labels[name] = self.pc()

    def done(self, extern):
        for i, f, l in self.fix:
            t = self.labels[l] if l in self.labels else extern[l]
            self.words[i] = f(self.at + 4 * i, t)
        return self.words


# ---------------------------------------------------------------- the module
def fixed_functions(at):
    """{name: (address, words)} of the helpers: CALLEE (AAPCS: saves x19/x20), CALLEE2 (reached by blr), END, END2
    (tail exit targets), BRX (a tail exit through br), SETJMP (musl-like), LONGJMP (HLE in C, real code for Unicorn)."""
    out = {}
    a = Asm(at)

    def fn(name, words):
        out[name] = (a.pc(), words)
        for w in words:
            a.put(w)

    fn('CALLEE', [stp_pre(19, 20, 31, -16), movz(1, 19, 0x1234), addsub_reg(1, 0, 0, 0, 0, 19),
                  addsub_reg(1, 0, 0, 20, 1, 2), logic_reg(1, 2, 1, 20, 3), logic_reg(1, 1, 2, 0, 1, 0, 3),
                  movz(1, 9, 77), addsub_reg(1, 1, 0, 15, 2, 9), addsub_reg(1, 1, 1, 31, 0, 1),   # cmp x0, x1
                  csel(1, 0, 1, 3, 0, 1, 11), ldp_post(19, 20, 31, 16), RET])
    fn('CALLEE2', [addsub_imm(1, 0, 0, 0, 0, 5), logic_reg(1, 2, 8, 8, 0), addsub_reg(0, 0, 1, 31, 0, 8), RET])
    # Tail exit targets. END and END2 read the flags they are entered with (into x16, x17), so the exit must publish
    # them; END3 sets its own first, so the exit need not (gen.py _entry_flags).
    fn('END', [mrs_nzcv(16), RET])
    # (an early exit from a test function's body: its epilogue, then a return)
    fn('END2', [mrs_nzcv(17), addsub_imm(1, 0, 0, 0, 0, 1), addsub_imm(1, 0, 0, 31, 31, FRAME),
                ldp_post(29, 30, 31, 16), RET])
    fn('END3', [addsub_reg(1, 1, 1, 31, 0, 1), mrs_nzcv(16), RET])   # cmp x0, x1
    end = out['END'][0]
    brx = a.pc()
    fn('BRX', [adr(13, brx, end), logic_reg(1, 1, 14, 31, 13), addsub_reg(1, 0, 1, 15, 0, 1), br(14)])
    fn('SETJMP', [stp(19, 20, 0, 0), stp(21, 22, 0, 16), stp(23, 24, 0, 32), stp(25, 26, 0, 48), stp(27, 28, 0, 64),
                  stp(29, 30, 0, 80), addsub_imm(1, 0, 0, 2, 31, 0), str_(2, 0, 104), movz(1, 0, 0), RET])
    fn('LONGJMP', [ldp(19, 20, 0, 0), ldp(21, 22, 0, 16), ldp(23, 24, 0, 32), ldp(25, 26, 0, 48), ldp(27, 28, 0, 64),
                   ldp(29, 30, 0, 80), ldr(2, 0, 104), addsub_imm(1, 0, 0, 31, 2, 0),
                   addsub_imm(0, 1, 1, 31, 1, 0), csel(0, 0, 1, 0, 1, 31, 1), br(30)])
    return out, a.pc()


def random_function(r, at, ext, feats):
    """Words of one random test function at `at`; ext: {name: address}. Returns (words, patch sites
    {address: label address})."""
    a = Asm(at)
    patches = {}
    a.put(stp_pre(29, 30, 31, -16))              # stp x29, x30, [sp, #-16]!
    a.put(addsub_imm(1, 1, 0, 31, 31, FRAME))   # sub sp, sp, #FRAME
    st = {'flags': True, 'n': 0, 'jmp': False}  # flags: what a reader sees is defined the same way in C and Unicorn

    def reg():
        return r.choice(POOL)

    def flag_def():
        sf, k = r.getrandbits(1), r.randrange(4)
        if k == 0:
            a.put(addsub_imm(sf, r.getrandbits(1), 1, r.choice(POOL + [31]), reg(), r.randrange(4096)))
        elif k == 1:
            a.put(addsub_reg(sf, r.getrandbits(1), 1, r.choice(POOL + [31]), reg(), reg(), r.randrange(3),
                             r.randrange(32 if not sf else 64)))
        elif k == 2:
            a.put(logic_reg(sf, 3, r.choice(POOL + [31]), reg(), reg(), r.randrange(4), r.randrange(32 if not sf else 64)))
        else:
            a.put(adc(sf, r.getrandbits(1), 1, reg(), reg(), reg()) if st['flags'] else
                  addsub_reg(sf, 1, 1, 31, reg(), reg()))
        st['flags'] = True

    def plain():
        sf, k = r.getrandbits(1), r.randrange(8)
        if k == 0:
            a.put(addsub_imm(sf, r.getrandbits(1), 0, reg(), reg(), r.randrange(4096)))
        elif k == 1:
            a.put(addsub_reg(sf, r.getrandbits(1), 0, reg(), reg(), reg(), r.randrange(3), r.randrange(32)))
        elif k == 2:
            a.put(logic_reg(sf, r.randrange(3), reg(), reg(), reg(), r.randrange(4), r.randrange(32), r.getrandbits(1)))
        elif k == 3:
            a.put(movz(sf, reg(), r.randrange(65536), r.randrange(2)))
        elif k == 4:
            a.put(str_(reg(), 31, 8 * r.randrange(16)))
        elif k == 5:
            a.put(ldr(reg(), 31, 8 * r.randrange(16)))
        elif k == 6:
            a.put(addsub_imm(1, 0, 0, reg(), 31, 8 * r.randrange(16)))   # add xd, sp, #imm
        else:
            a.put(stp(reg(), reg(), 31, 8 * r.randrange(15)))

    def reader():
        k = r.randrange(5)
        sf = r.getrandbits(1)
        if k == 0:
            a.put(csel(sf, r.getrandbits(1), r.getrandbits(1), reg(), reg(), reg(), r.randrange(14)))
        elif k == 1:
            a.put(ccmp_imm(sf, r.getrandbits(1), reg(), r.randrange(32), r.randrange(14), r.randrange(16)))
        elif k == 2:
            a.put(ccmp_reg(sf, r.getrandbits(1), reg(), reg(), r.randrange(14), r.randrange(16)))
        elif k == 3:
            a.put(adc(sf, r.getrandbits(1), r.getrandbits(1), reg(), reg(), reg()))
        else:
            a.put(mrs_nzcv(reg()))

    def skip(body):
        """A forward branch over body(): b.cond (when flags are known), cbz or cbnz."""
        st['n'] += 1
        lab = f's{st["n"]}'
        if st['flags'] and r.random() < 0.6:
            cond = r.randrange(14)
            a.ref(lambda pc, t, c=cond: bcond(pc, t, c), lab)
        else:
            sf, op, rt = r.getrandbits(1), r.getrandbits(1), reg()
            a.ref(lambda pc, t, s=sf, o=op, x=rt: cbz(s, o, x, pc, t), lab)
        flags_before = st['flags']
        body()
        a.label(lab)
        st['flags'] = st['flags'] and flags_before

    def item(depth=0):
        k = r.random()
        if k < 0.35:
            plain()
        elif k < 0.55:
            flag_def()
        elif k < 0.70:
            if st['flags']:
                reader()
            else:
                flag_def()
        elif k < 0.78 and depth < 2:
            skip(lambda: [item(depth + 1) for _ in range(r.randrange(1, 4))])
        elif k < 0.82 and 'call' in feats:
            a.ref(bl, 'CALLEE')
            st['flags'] = False   # (a callee's flags: AAPCS64 code never reads them)
        elif k < 0.85 and 'call' in feats:
            a.ref(lambda pc, t: adr(12, pc, t), 'CALLEE2')
            a.put(blr(12))
            st['flags'] = False
        elif k < 0.89 and 'svc' in feats:
            a.put(svc(SVC_STUB))
        elif k < 0.93 and 'patch' in feats:
            if not st['flags']:
                flag_def()   # (its C reads the flags)
            st['n'] += 1
            lab = f'p{st["n"]}'
            patches[a.pc()] = lab
            a.put(svc(SVC_PATCH))   # (in C: the patch site's text)
            item(depth + 1)
            flags_before = True
            a.label(lab)
            st['flags'] = st['flags'] and flags_before
            a.put(addsub_imm(1, 0, 0, 0, 0, 0))   # a label needs an instruction (mov x0, x0)
        elif k < 0.96 and 'exit' in feats and st['flags'] and depth == 0:
            a.ref(lambda pc, t, c=r.randrange(14): bcond(pc, t, c), 'END2')   # early tail exit
        else:
            plain()

    if 'loop' in feats and r.random() < 0.5:
        a.put(movz(1, COUNTER, r.randrange(1, 4)))
        a.label('loop')
        for _ in range(r.randrange(2, 6)):
            item(1)
        a.put(addsub_imm(1, 1, 1, COUNTER, COUNTER, 1))   # subs x11, x11, #1
        a.ref(lambda pc, t: bcond(pc, t, 1), 'loop')       # b.ne loop
        st['flags'] = True
    for _ in range(r.randrange(4, 30)):
        item()
    if 'setjmp' in feats and r.random() < 0.5:
        a.put(addsub_imm(1, 0, 0, 0, 31, 0x80))   # add x0, sp, #0x80
        a.ref(bl, 'SETJMP')
        a.ref(lambda pc, t: cbz(0, 1, 0, pc, t), 'after')   # cbnz w0, after
        st['flags'] = False
        for _ in range(r.randrange(2, 12)):
            item(1)
        a.put(movz(1, 1, r.randrange(3)))          # longjmp(buf, 0..2)
        a.put(addsub_imm(1, 0, 0, 0, 31, 0x80))
        a.ref(bl, 'LONGJMP')
        a.label('after')
        st['flags'] = False
        for _ in range(r.randrange(1, 8)):
            item(1)
    flag_def()   # (the exit publishes these: a callee's flags are not the caller's in C)
    a.put(addsub_imm(1, 0, 0, 31, 31, FRAME))   # add sp, sp, #FRAME
    a.put(ldp_post(29, 30, 31, 16))              # ldp x29, x30, [sp], #16
    a.ref(b, 'BRX' if 'exit' in feats and r.random() < 0.3 else r.choice(['END', 'END3']))
    words = a.done(ext)
    return words, {p: (a.labels[l] if l in a.labels else ext[l]) for p, l in patches.items()}


class SynthModule(gen.Module):
    """gen.Module over hand-assembled words, without an NSO."""

    def __init__(self, words, locals_):
        self.name, self.base = 'test', CODE
        self.lo, self.hi = CODE, CODE + 4 * len(words)
        self.words = words
        self.lifter = Lifter(CODE, 'c')
        self.cache, self.stats, self.mu, self.cs = {}, Counter(), None, None
        self.fdes, self.fde_starts, self.jt_cache = [], [], {}
        self.setjmps, self.sites, self.patches = set(), {}, {}
        self.locals = locals_
        self.entry_flags = {}


def patch_text(label):
    def f(m, a):
        return ('{ extern void rt_test_patch(Cpu *); c->x[21] += 5; rt_test_patch(c); '
                f'if (c->x[9] & 1) goto L_{label:x}; }}')
    return f


def generate(words, starts, patches, setjmp, longjmp, locals_):
    m = SynthModule(words, locals_)
    m.setjmps = {setjmp}
    m.patches = {a: patch_text(t) for a, t in patches.items()}
    hle = {longjmp: 'longjmp'}
    out = ['#include "cpu.h"', '#include <setjmp.h>', 'jmp_buf *rt_setjmp_slot(Cpu *c, uint64_t gbuf);',
           'void hle_longjmp(Cpu *c);']
    out += [f'void {gen.fname(s)}(Cpu *c);' for s in starts]
    for i, s in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else m.hi
        labels, ext, tables = m.scan(s, end)
        labels |= {t for p, t in patches.items() if s <= p < end}
        out += m.emit_function(s, end, labels, tables, hle)
    out.append('const uint64_t fn_addrs[] = {' + ', '.join(hx(s) for s in starts) + '};')
    out.append('const GuestFn fn_ptrs[] = {' + ', '.join(gen.fname(s) for s in starts) + '};')
    out.append(f'const int fn_count = {len(starts)};')
    return '\n'.join(out) + '\n', m.stats


# ---------------------------------------------------------------- Unicorn
def emulate(mu, entry, x, sp, nzcv, patches):
    mu.mem_write(DATA, bytes(DATA_SIZE))
    for i in range(29):
        mu.reg_write(A.UC_ARM64_REG_X0 + i, x[i])
    mu.reg_write(A.UC_ARM64_REG_X29, x[29])
    mu.reg_write(A.UC_ARM64_REG_X30, STOP)
    mu.reg_write(A.UC_ARM64_REG_SP, sp)
    mu.reg_write(A.UC_ARM64_REG_NZCV, nzcv)
    pc, steps = entry, 0
    while pc != STOP:
        mu.emu_start(pc, STOP, count=20000)
        pc = mu.reg_read(A.UC_ARM64_REG_PC)
        steps += 1
        if steps > 1000:
            raise RuntimeError('runaway')
        if pc == STOP:
            break
        if mu.svc_hit is not None:
            pc = mu.svc_hit
            mu.svc_hit = None
            continue
        raise RuntimeError(f'stopped at {pc:x}')
    ox = [mu.reg_read(A.UC_ARM64_REG_X0 + i) for i in range(29)]
    ox += [mu.reg_read(A.UC_ARM64_REG_X29), mu.reg_read(A.UC_ARM64_REG_X30)]
    return ox, mu.reg_read(A.UC_ARM64_REG_SP), mu.reg_read(A.UC_ARM64_REG_NZCV) & 0xF0000000, \
        zlib.crc32(bytes(mu.mem_read(DATA, DATA_SIZE)))


def make_unicorn(code, patches):
    mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
    mu.mem_map(CODE, CODE_SIZE)
    mu.mem_write(CODE, code)
    mu.mem_map(DATA, DATA_SIZE)
    mu.svc_hit = None

    def R(n):
        return mu.reg_read(A.UC_ARM64_REG_X0 + n if n < 29 else A.UC_ARM64_REG_X29 if n == 29 else A.UC_ARM64_REG_X30)

    def W(n, v):
        mu.reg_write(A.UC_ARM64_REG_X0 + n if n < 29 else A.UC_ARM64_REG_X29 if n == 29 else A.UC_ARM64_REG_X30,
                     v & M64)

    def intr(mu_, intno, data):
        pc = mu.reg_read(A.UC_ARM64_REG_PC)   # (the instruction after the svc)
        imm = (struct.unpack('<I', bytes(mu.mem_read(pc - 4, 4)))[0] >> 5) & 0xFFFF
        nxt = pc
        if imm == SVC_STUB:   # as the harness's rt_svc
            x0 = R(0) + 3 * R(19) + R(1)
            W(1, R(20) ^ R(2))
            W(0, x0)
            W(7, R(28) + 1)
            W(21, R(21) + 1)
        elif imm == SVC_PATCH:   # as the patch site's C and the harness's rt_test_patch
            W(21, R(21) + 5)
            nz = mu.reg_read(A.UC_ARM64_REG_NZCV) & 0xF0000000
            W(0, R(3) + R(19) + (nz >> 28))
            W(9, R(0) ^ R(4))
            mu.reg_write(A.UC_ARM64_REG_NZCV, nz ^ 0x20000000)
            if R(9) & 1:
                nxt = patches[pc - 4]
        else:
            raise RuntimeError(f'svc {imm:x}')
        mu.svc_hit = nxt
        mu.emu_stop()

    mu.hook_add(uc.UC_HOOK_INTR, intr)
    return mu


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "cpu.h"
extern const uint64_t fn_addrs[];
extern const GuestFn fn_ptrs[];
extern const int fn_count;
volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }
uint64_t rt_ticks(void) { return 0; }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; fprintf(stderr, "brk %x at %llx\n", imm, (unsigned long long)pc); exit(3); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; fprintf(stderr, "udf at %llx\n", (unsigned long long)pc); exit(3); }
void rt_dispatch(Cpu *c) {
    for (int i = 0; i < fn_count; i++) if (fn_addrs[i] == c->pc) { fn_ptrs[i](c); return; }
    fprintf(stderr, "dispatch to %llx\n", (unsigned long long)c->pc); exit(3);
}
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {  // (test_locals.py intr: SVC_STUB)
    (void)pc;
    if (imm != 0x7f) { fprintf(stderr, "svc %x\n", imm); exit(3); }
    uint64_t x0 = c->x[0] + 3 * c->x[19] + c->x[1];
    c->x[1] = c->x[20] ^ c->x[2];
    c->x[0] = x0;
    c->x[7] = c->x[28] + 1;
    c->x[21] += 1;
}
void rt_test_patch(Cpu *c) {  // (test_locals.py intr: SVC_PATCH, after the C's own x21 += 5)
    uint64_t nz = c->nzcv & 0xf0000000ull;
    c->x[0] = c->x[3] + c->x[19] + (nz >> 28);
    c->x[9] = c->x[0] ^ c->x[4];
    c->nzcv = nz ^ 0x20000000ull;
}
static uint64_t slot_gbuf;
static jmp_buf slot;
jmp_buf *rt_setjmp_slot(Cpu *c, uint64_t gbuf) { (void)c; slot_gbuf = gbuf; return &slot; }
void hle_longjmp(Cpu *c) {  // (test_locals.py LONGJMP, as runtime/hle/setjmp.c restores it)
    uint64_t gbuf = c->x[0];
    uint32_t val = (uint32_t)c->x[1];
    if (gbuf != slot_gbuf) { fprintf(stderr, "longjmp to %llx\n", (unsigned long long)gbuf); exit(3); }
    for (int r = 19; r <= 30; r++) c->x[r] = LD64(gbuf + 8 * (r - 19));
    c->x[2] = c->sp = LD64(gbuf + 0x68);
    (void)subs32(c, val, 0);
    c->x[0] = val ? val : 1;
    longjmp(slot, 1);
}
static uint32_t crc_tab[256];
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t k = i;
        for (int j = 0; j < 8; j++) k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1;
        crc_tab[i] = k;
    }
    uint64_t data = 0x7310000000ull, size = 0x10000;
#ifdef _WIN32
    void *p = VirtualAlloc((void *)data, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)data, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
#endif
    if (p != (void *)data) { fprintf(stderr, "map failed\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    struct { uint64_t k, x[31], sp, nzcv; } in;
    static Cpu c;
    while (fread(&in, sizeof in, 1, f) == 1) {
        memset((void *)data, 0, size);
        memset(&c, 0, sizeof c);
        memcpy(c.x, in.x, sizeof in.x);
        c.x[30] = 0x72fffff000ull;  // (STOP)
        c.sp = in.sp;
        c.nzcv = in.nzcv;
        fn_ptrs[in.k](&c);
        uint32_t crc = 0xffffffffu;
        for (uint64_t i = 0; i < size; i++) crc = crc_tab[(crc ^ ((uint8_t *)data)[i]) & 0xff] ^ (crc >> 8);
        for (int i = 0; i < 31; i++) printf("%llx ", (unsigned long long)c.x[i]);
        printf("%llx %llx %08x\n", (unsigned long long)c.sp, (unsigned long long)(c.nzcv & 0xf0000000ull), crc ^ 0xffffffffu);
    }
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument('--functions', type=int, default=400)
    ap.add_argument('--cases', type=int, default=6)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--reg-locals', type=int, default=gen.regs.LOCALS)
    ap.add_argument('--no-callee-saved-trust', action='store_true')
    ap.add_argument('--show', type=int, default=10)
    args = ap.parse_args()
    r = random.Random(args.seed)
    fixed, at = fixed_functions(CODE)
    ext = {k: v[0] for k, v in fixed.items()}
    words = [w for _, ws in sorted(fixed.values()) for w in ws]
    starts = sorted(ext.values())
    tests, patches = [], {}
    feats_all = ['call', 'svc', 'patch', 'setjmp', 'exit', 'loop']
    for k in range(args.functions):
        feats = set(feats_all) if k % 4 else set(r.sample(feats_all, r.randrange(len(feats_all) + 1)))
        ws, ps = random_function(r, CODE + 4 * len(words), ext, feats)
        tests.append(CODE + 4 * len(words))
        starts.append(CODE + 4 * len(words))
        words += ws
        patches.update(ps)
    code = struct.pack(f'<{len(words)}I', *words)
    work = os.path.join(ROOT, 'build', 'test_locals')
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, 'harness.c'), 'w') as f:
        f.write(HARNESS.replace('0x72fffff000ull', hx(STOP)))
    cc = os.environ.get('CC', 'clang')
    native = ['-march=x86-64-v3'] if platform.machine().lower() in ('amd64', 'x86_64') else []
    flags_c = ['-O1'] + native + ['-fno-strict-aliasing', '-Wno-unused-label', '-Wno-return-type', '-I',
                                  os.path.join(ROOT, 'runtime')]
    loc = gen.Locals(args.reg_locals, not args.no_callee_saved_trust)
    exes = {}
    for mode, lc in (('off', None), ('locals', loc)):
        text, stats = generate(words, starts, patches, ext['SETJMP'], ext['LONGJMP'], lc)
        src = os.path.join(work, f'fns_{mode}.c')
        with open(src, 'w') as f:
            f.write(text)
        exe = os.path.join(work, f'harness_{mode}' + ('.exe' if os.name == 'nt' else ''))
        subprocess.check_call([cc] + flags_c + ['-o', exe, os.path.join(work, 'harness.c'), src])
        exes[mode] = exe
        if lc:
            print('locals:', {k: v for k, v in sorted(stats.items()) if k.startswith(('local_', 'flag_', 'reg_'))})

    mu = make_unicorn(code, patches)
    cases, expect = [], []
    index = {s: i for i, s in enumerate(starts)}
    for t in tests:
        for _ in range(args.cases):
            x = [r.choice([0, 1, 2, 3, 0xFFFFFFFF, M64, 1 << 63, 0x80000000]) if r.random() < 0.3 else
                 r.getrandbits(r.choice([8, 32, 64])) for _ in range(30)] + [0]
            nzcv = r.getrandbits(4) << 28
            cases.append((index[t], x, SP0, nzcv))
            expect.append(emulate(mu, t, x, SP0, nzcv, patches))
    inp = os.path.join(work, 'cases.bin')
    with open(inp, 'wb') as f:
        for k, x, sp, nzcv in cases:
            f.write(struct.pack('<Q31QQQ', k, *x, sp, nzcv))
    bad = 0
    for mode, exe in exes.items():
        p = subprocess.run([exe, inp], capture_output=True)
        lines = p.stdout.decode().split('\n')
        if p.returncode:
            print(f'{mode}: harness failed {p.returncode}: {p.stderr.decode()[-500:]}')
            bad += 1
        fails = []
        for (k, x, sp, nzcv), (ex, esp, enz, ecrc), line in zip(cases, expect, lines):
            parts = line.split()
            got = [int(v, 16) for v in parts[:33]] + [int(parts[33], 16)]
            want = ex + [esp, enz, ecrc]
            # (NZCV after the return is not compared: AAPCS64 keeps no flags across a return, and a function with
            # locals does not publish them there. The exit targets copy the flags they see into x16 / x17.)
            d = [f'{n}: got {g:x} want {w:x}' for n, g, w in
                 zip([f'x{i}' for i in range(31)] + ['sp', 'nzcv', 'mem'], got, want) if g != w and n != 'nzcv']
            if d:
                fails.append((starts[k], d))
        ran = sum(1 for l in lines if l.strip())
        print(f'{mode}: {len(tests)} functions, {ran}/{len(cases)} cases ran, {len(fails)} failing '
              f'({len({f for f, _ in fails})} functions)')
        for fn, d in fails[:args.show]:
            print(f'  f_{fn:x}: {"; ".join(d[:4])}')
        bad += bool(fails) or ran != len(cases)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
