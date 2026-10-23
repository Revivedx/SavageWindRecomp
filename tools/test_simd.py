"""Differential test of the portable FP/SIMD lowering (lifter --simd=c) against Unicorn (Cortex-A57).

No NSO needed: the FP/SIMD data-processing space (bits 27..25 = 111), the FP/SIMD load/store space and CRC32
are enumerated (every opcode bit combination, random register fields), each encoding's validity is compared
between Unicorn and the lifter, then a stratified sample of valid encodings is compiled (clang) and run with
random interesting operands (NaNs, infinities, denormals, +-0, saturation edges, random FPCR.DN/FZ/RMode/AHP)
against Unicorn: X0-X30, SP, NZCV, V0-V31 and (for stores) memory must match. FPSR is not compared (the
lowering doesn't model the cumulative exception bits or QC).

Usage: test_simd.py [--encodings N] [--cases K] [--seed S] [--jobs J] [--no-validity] [--filter HEX/MASK]
"""
import argparse
import functools
import os
import platform
import random
import struct
import subprocess
import sys
import time
import zlib
from collections import Counter, defaultdict

import unicorn as uc
from unicorn import arm64_const as A

from lifter import Lifter, Unsupported

print = functools.partial(print, flush=True)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUF, BUF_SIZE = 0x7200000000, 0x20000
BASE_AREA = BUF + 0x8000
CODE, CODE_SIZE = 0x7210000000, 0x400000
IN_FMT = '<QQ32QQQ'
OUT_SIZE = 31 * 8 + 3 * 8 + 32 * 16
UC_INVALID = (uc.UC_ERR_INSN_INVALID, uc.UC_ERR_EXCEPTION)

F32 = [0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fa00001, 0xff812345, 0x7fd00abc,
       0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00400000, 0x00800000, 0x80800000, 0x00800001, 0x7f7fffff,
       0xff7fffff, 0x3f800000, 0xbf800000, 0x3f000000, 0xbf000000, 0x40000000, 0x40400000, 0x3fc00000, 0xbfc00000,
       0x40200000, 0x40100000, 0x4f000000, 0xcf000000, 0x4f800000, 0x5f000000, 0xdf000000, 0x5f800000, 0x4effffff,
       0x4f7fffff, 0x5effffff, 0x5f7fffff, 0x3effffff, 0x3f7fffff, 0x7effffff, 0x01000000, 0x3e800000, 0x7e800000,
       0x7e7fffff, 0x00200000, 0x001fffff, 0x00200001, 0x47800000, 0x477fe000, 0x477ff000, 0x38800000, 0x33800000,
       0x33000000, 0x387fc000, 0x3f7ff000, 0x46fffe00, 0x80000000 | 0x00200000, 0x7f000000, 0x7f7ffffe]
F64 = [0, 1 << 63, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000000,
       0x7ff4000000000001, 0xfff0000000012345, 0x7ffc00000abcdef0, 1, (1 << 63) | 1, 0x000fffffffffffff,
       0x0008000000000000, 0x0010000000000000, 0x8010000000000000, 0x7fefffffffffffff, 0xffefffffffffffff,
       0x3ff0000000000000, 0xbff0000000000000, 0x3fe0000000000000, 0x4000000000000000, 0x4008000000000000,
       0x3ff8000000000000, 0xbff8000000000000, 0x4004000000000000, 0x41e0000000000000, 0xc1e0000000000000,
       0x41f0000000000000, 0x43e0000000000000, 0xc3e0000000000000, 0x43f0000000000000, 0x41dfffffffc00000,
       0x41efffffffe00000, 0x43dfffffffffffff, 0x43efffffffffffff, 0x3fdfffffffffffff, 0x7fdfffffffffffff,
       0x7fd0000000000000, 0x0004000000000000, 0x0003ffffffffffff, 0x47efffffe0000000, 0x47efffff00000000,
       0x36a0000000000000, 0x3810000000000000, 0x380fffffffffffff, 0x47f0000000000000, 0x3e70000000000000,
       0x3f0ffc0000000000, 0x40effc0000000000, 0x40effe0000000000, 0x3e6ffffffff00000, 0x369fffffffffffff,
       0x3810000010000000, 0x381000000fffffff, 0x3e80000000000000]
F16 = [0, 0x8000, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7d00, 0x7e55, 0x0001, 0x8001, 0x03ff, 0x0400, 0x7bff, 0xfbff,
       0x3c00, 0xbc00, 0x3800, 0x4000, 0x3e00, 0x7fff, 0xffff]


def rand_lane(r, sz):
    """Random interesting integer lane of 8 << sz bits."""
    bits_ = 8 << sz
    k = r.random()
    if k < 0.35:
        return r.choice([0, 1, 2, 3, (1 << bits_) - 1, (1 << bits_) - 2, 1 << (bits_ - 1), (1 << (bits_ - 1)) - 1,
                         (1 << (bits_ - 1)) + 1, 0x80 % (1 << bits_), 0x7f, bits_ - 1, bits_, bits_ + 1,
                         (1 << bits_) - bits_, 1 << (bits_ // 2)])
    if k < 0.5:
        return r.getrandbits(r.randint(1, bits_))
    if k < 0.6:
        return ((1 << bits_) - r.getrandbits(r.randint(1, bits_ - 1))) & ((1 << bits_) - 1)
    return r.getrandbits(bits_)


def rand_f32(r):
    k = r.random()
    if k < 0.4:
        return r.choice(F32)
    if k < 0.8:
        e = r.choice([r.randint(110, 145), r.randint(0, 255), r.randint(120, 135)])
        return (r.getrandbits(1) << 31) | (e << 23) | (r.getrandbits(23) if r.random() < 0.7 else
                                                        r.getrandbits(4) << 19)
    return struct.unpack('<I', struct.pack('<f', r.choice([r.uniform(-4, 4), float(r.randint(-300, 300)) + 0.5,
                                                           float(r.randint(-5, 5)), r.uniform(-1e6, 1e6)])))[0]


def rand_f64(r):
    k = r.random()
    if k < 0.4:
        return r.choice(F64)
    if k < 0.8:
        e = r.choice([r.randint(1000, 1050), r.randint(0, 2047), r.randint(1020, 1030), r.randint(0, 40),
                      r.randint(2000, 2047), r.randint(860, 920)])
        return (r.getrandbits(1) << 63) | (e << 52) | (r.getrandbits(52) if r.random() < 0.7 else
                                                        r.getrandbits(6) << 46)
    return struct.unpack('<Q', struct.pack('<d', r.choice([r.uniform(-4, 4), float(r.randint(-300, 300)) + 0.5,
                                                           r.uniform(-1e12, 1e12)])))[0]


def rand_v(r):
    k = r.random()
    out = 0
    if k < 0.3:
        for i in range(4):
            out |= rand_f32(r) << (32 * i)
    elif k < 0.55:
        for i in range(2):
            out |= rand_f64(r) << (64 * i)
    elif k < 0.6:
        for i in range(8):
            out |= (r.choice(F16) if r.random() < 0.5 else r.getrandbits(16)) << (16 * i)
    elif k < 0.9:
        sz = r.randrange(4)
        for i in range(16 >> sz):
            out |= rand_lane(r, sz) << ((8 << sz) * i)
    else:
        out = r.getrandbits(128)
    if r.random() < 0.15:  # upper half zero, like most 64-bit results
        out &= (1 << 64) - 1
    return out


def rand_x(r):
    k = r.random()
    if k < 0.3:
        return r.choice([0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFF,
                         0x7FFFFFFF, 0x80000000, 0x100000000, 0xFFFFFFFF80000000, 0x8000000000000001, 0xFFFFFF,
                         0x1000001, 0x20000000000001, 0xFFFFFFFFFF000001])
    if k < 0.45:
        return rand_f32(r) | (r.getrandbits(32) << 32 if r.random() < 0.3 else 0)
    if k < 0.6:
        return rand_f64(r)
    if k < 0.75:
        return r.getrandbits(r.randint(1, 64))
    if k < 0.8:
        return (-r.getrandbits(r.randint(1, 63))) & 0xFFFFFFFFFFFFFFFF
    return r.getrandbits(64)


def rand_fpcr(r):
    k = r.random()
    if k < 0.55:
        return 0
    v = 0
    if r.random() < 0.4:
        v |= 1 << 25  # DN
    if r.random() < 0.4:
        v |= 1 << 24  # FZ
    if r.random() < 0.4:
        v |= r.randrange(4) << 22  # RMode
    if r.random() < 0.15:
        v |= 1 << 26  # AHP
    return v


def is_ldst(w):
    return (w >> 25) & 1 == 0 and (w >> 27) & 1 == 1


def is_store(w):
    if not is_ldst(w):
        return False
    if (w & 0x3B000000) == 0x18000000:  # literal
        return False
    if (w & 0xBE000000) == 0x0C000000:  # structures: L = bit 22
        return not (w >> 22) & 1
    if (w >> 27) & 7 == 0b101:  # pair
        return not (w >> 22) & 1
    return not (w >> 22) & 1


def is_literal(w):
    return (w & 0x3F000000) == 0x1C000000


def class_key(w):
    """Opcode signature: register fields (Rd, Rn, Rm) cleared."""
    if is_ldst(w):
        return w & 0xFFE0FC00 if (w & 0xBE000000) == 0x0C000000 else w & 0xFFFFFC00
    return w & 0xFFE0FC00


def candidates(r):
    """Every opcode bit pattern of the FP/SIMD data-processing and load/store spaces, random registers."""
    out = []
    for p in range(1 << 19):
        hi, mid = p >> 15, p & 0x7FFF
        rr = r.getrandbits(10)
        out.append((hi << 28) | (0b111 << 25) | (mid << 10) | rr)  # data processing
        out.append((hi << 28) | (1 << 27) | (1 << 26) | (mid << 10) | rr)  # loads/stores
    for sf in (0, 1):
        for C in (0, 1):
            for sz in range(4):
                for _ in range(4):
                    out.append((sf << 31) | (0b0011010110 << 21) | (r.getrandbits(5) << 16) | (0b010 << 13) |
                               (C << 12) | (sz << 10) | r.getrandbits(10))
    return out


class Emu:
    def __init__(self):
        mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        mu.ctl_set_cpu_model(A.UC_CPU_ARM64_A57)
        mu.mem_map(BUF, BUF_SIZE)
        mu.mem_map(CODE, CODE_SIZE)
        mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
        self.mu = mu
        self.xregs = [A.UC_ARM64_REG_X0 + i for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
        self.qregs = [A.UC_ARM64_REG_Q0 + i for i in range(32)]
        self.out_regs = self.xregs + [A.UC_ARM64_REG_SP, A.UC_ARM64_REG_NZCV] + self.qregs

    def run(self, pc, fpcr, x, sp, nzcv, v):
        mu = self.mu
        mu.reg_write_batch(list(zip(self.xregs, x[:31])) + list(zip(self.qregs, v)) +
                           [(A.UC_ARM64_REG_SP, sp), (A.UC_ARM64_REG_NZCV, nzcv), (A.UC_ARM64_REG_FPCR, fpcr)])
        mu.emu_start(pc, pc + 4, count=1)
        o = mu.reg_read_batch(self.out_regs)
        return o[:31], o[31], o[32] & 0xF0000000, o[33:]


def validity(lifter, ws, emu):
    """Map word -> (lifter ok, unicorn ok); words are placed at CODE + 4i."""
    mu = emu.mu
    res = {}
    step = CODE_SIZE // 4
    for base in range(0, len(ws), step):
        chunk = ws[base:base + step]
        mu.mem_write(CODE, struct.pack(f'<{len(chunk)}I', *chunk))
        mu.ctl_remove_cache(CODE, CODE + CODE_SIZE)
        state = list(zip(emu.xregs, [BASE_AREA] * 31)) + [(A.UC_ARM64_REG_SP, BASE_AREA)]
        for i, w in enumerate(chunk):
            pc = CODE + 4 * i
            try:
                lifter.lift(w, pc)
                lok = True
            except Unsupported:
                lok = False
            if i % 32 == 0:
                mu.reg_write_batch(state)
            try:
                mu.emu_start(pc, pc + 4, count=1)
                uok = True
            except uc.UcError as e:
                uok = e.errno not in UC_INVALID
            res[w] = (lok, uok)
    return res


def make_case(r, w):
    fpcr = rand_fpcr(r)
    v = [rand_v(r) for _ in range(32)]
    nzcv = r.getrandbits(4) << 28
    sp = BASE_AREA + r.randrange(0, 0x100, 16)
    if is_ldst(w):
        x = [BASE_AREA + r.randrange(0, 0x100) for _ in range(31)]
        rm = (w >> 16) & 31
        if rm < 31 and rm != (w >> 5) & 31:
            reg_off = (w & 0x3B200C00) == 0x38200800  # register offset
            post_reg = (w & 0xBF800000) in (0x0C800000, 0x0D800000)
            if reg_off or post_reg:
                x[rm] = r.randrange(0, 0x100)
    else:
        x = [rand_x(r) for _ in range(31)]
    return fpcr, x, sp, nzcv, v


def compile_tests(work, bodies, jobs, cflags):
    cc = os.environ.get('CC', 'clang')
    flags = ['-O1'] + cflags.split() + ['-fno-strict-aliasing', '-fdenormal-fp-math=dynamic', '-Wno-unused-label', '-I',
             os.path.join(ROOT, 'runtime')]
    n = max(1, min(jobs, len(bodies) // 200 + 1))
    per = (len(bodies) + n - 1) // n
    srcs, procs = [], []
    for j in range(n):
        path = os.path.join(work, f'tests_{j}.c')
        with open(path, 'w') as f:
            f.write('#include "cpu.h"\n')
            for k in range(j * per, min(len(bodies), (j + 1) * per)):
                f.write(f'void t{k}(Cpu *c) {{ {bodies[k]} }}\n')
        srcs.append(path)
    with open(os.path.join(work, 'table.c'), 'w') as f:
        f.write('#include "cpu.h"\n')
        f.write(''.join(f'void t{k}(Cpu *c);\n' for k in range(len(bodies))))
        f.write('GuestFn tests[] = {' + ','.join(f't{k}' for k in range(len(bodies))) + '};\n')
        f.write(f'int ntests = {len(bodies)};\n')
    srcs += [os.path.join(work, 'table.c'), os.path.join(ROOT, 'tools', 'test_simd_harness.c')]
    objs = []
    for s in srcs:
        o = s[:-2] + '.o' if s.startswith(work) else os.path.join(work, 'harness.o')
        objs.append(o)
        procs.append(subprocess.Popen([cc] + flags + ['-c', s, '-o', o]))
    for p in procs:
        if p.wait():
            raise SystemExit('compile failed')
    exe = os.path.join(work, 'harness.exe' if os.name == 'nt' else 'harness')
    subprocess.check_call([cc, '-o', exe] + objs)
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--encodings', type=int, default=20000)
    ap.add_argument('--cases', type=int, default=8)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--no-validity', action='store_true', help='skip the full validity enumeration')
    ap.add_argument('--filter', help='only encodings with (w & MASK) == VAL, as VAL/MASK in hex')
    ap.add_argument('--show', type=int, default=40)
    ap.add_argument('--cflags', default='-march=x86-64-v3' if platform.machine().lower() in ('amd64', 'x86_64') else '',
                    help='extra compiler flags for the lifted tests')
    args = ap.parse_args()
    r = random.Random(args.seed)
    t0 = time.time()
    lifter = Lifter(0, 'c')
    emu = Emu()
    cands = candidates(r)
    if args.filter:
        val, mask = (int(x, 16) for x in args.filter.split('/'))
        cands = [w for w in cands if (w & mask) == val]
    if args.no_validity:
        sample = r.sample(cands, min(len(cands), args.encodings * 4))
        val = validity(lifter, sample, emu)
    else:
        val = validity(lifter, cands, emu)
    agree = Counter()
    only = defaultdict(list)
    for w, (lok, uok) in val.items():
        agree[(lok, uok)] += 1
        if lok != uok:
            only['lifter' if lok else 'unicorn'].append(w)
    print(f'validity: {len(val)} encodings, both valid {agree[(True, True)]}, both invalid {agree[(False, False)]}, '
          f'lifter-only {agree[(True, False)]}, unicorn-only {agree[(False, True)]} ({time.time() - t0:.0f}s)')
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    def dis(w):
        d = next(md.disasm(struct.pack('<I', w), 0), None)
        return f'{d.mnemonic} {d.op_str}' if d else '?'

    for kind in ('lifter', 'unicorn'):
        ws = only[kind]
        if ws:
            byname = Counter(dis(w).split(' ')[0] for w in ws)
            print(f'  {kind}-only by mnemonic: {dict(byname.most_common(30))}')
            for w in ws[:args.show // 2]:
                print(f'    {w:08x} {dis(w)}')

    # stratified sample of valid encodings: one per opcode signature first
    valid = [w for w, (lok, uok) in val.items() if lok and uok]
    r.shuffle(valid)
    seen, pick = set(), []
    for w in valid:
        k = class_key(w)
        if k not in seen:
            seen.add(k)
            pick.append(w)
    picked = set(pick)
    rest = [w for w in valid if w not in picked]
    pick = (pick + rest)[:args.encodings]
    # place at CODE + 4i; literal loads get an offset into the code region
    code = []
    for i, w in enumerate(pick):
        pc = CODE + 4 * i
        if is_literal(w):
            tgt = CODE + r.randrange(0, (len(pick) * 4 + 0x1000) & ~3, 4)
            w = (w & ~(0x7FFFF << 5)) | ((((tgt - pc) >> 2) & 0x7FFFF) << 5)
            pick[i] = w
        code.append(w)
    code_bytes = struct.pack(f'<{len(code)}I', *code) + bytes(0x1000 + 64)
    bodies = []
    for i, w in enumerate(pick):
        ins = lifter.lift(w, CODE + 4 * i)
        bodies.append(ins.code)
    work = os.path.join(ROOT, 'build', 'test_simd')
    os.makedirs(work, exist_ok=True)
    t1 = time.time()
    exe = compile_tests(work, bodies, args.jobs, args.cflags)
    print(f'compiled {len(bodies)} tests ({time.time() - t1:.0f}s)')

    mu = emu.mu
    mu.mem_write(CODE, code_bytes)
    mu.ctl_remove_cache(CODE, CODE + CODE_SIZE)
    buf0 = bytes(r.getrandbits(8) for _ in range(4096)) * (BUF_SIZE // 4096)
    mu.mem_write(BUF, buf0)
    with open(os.path.join(work, 'code.bin'), 'wb') as f:
        f.write(code_bytes)
    with open(os.path.join(work, 'buf.bin'), 'wb') as f:
        f.write(buf0)
    cases, expect = [], []
    t2 = time.time()
    for k, w in enumerate(pick):
        pc = CODE + 4 * k
        st = is_store(w)
        for _ in range(args.cases):
            fpcr, x, sp, nzcv, v = make_case(r, w)
            try:
                res = emu.run(pc, fpcr, x, sp, nzcv, v)
            except uc.UcError:
                if st:
                    mu.mem_write(BUF, buf0)
                continue
            mh = 0
            if st:
                mh = zlib.crc32(bytes(mu.mem_read(BUF, BUF_SIZE)))
                mu.mem_write(BUF, buf0)
            cases.append((k, fpcr, x, sp, nzcv, v, st))
            expect.append(res + (mh,))
    print(f'unicorn: {len(cases)} cases ({time.time() - t2:.0f}s)')
    with open(os.path.join(work, 'cases.bin'), 'wb') as f:
        for k, fpcr, x, sp, nzcv, v, st in cases:
            f.write(struct.pack(IN_FMT, k, fpcr, *(x + [0]), sp, nzcv))
            for q in v:
                f.write(q.to_bytes(16, 'little'))
            f.write(struct.pack('<QQ', int(st), 0))
    outp = os.path.join(work, 'out.bin')
    t3 = time.time()
    p = subprocess.run([exe] + [os.path.join(work, n) for n in ('code.bin', 'buf.bin', 'cases.bin')] + [outp],
                       capture_output=True)
    if p.returncode:
        print('harness failed', p.returncode, p.stderr.decode()[-2000:])
    data = open(outp, 'rb').read()
    print(f'harness: {len(data) // OUT_SIZE} cases ({time.time() - t3:.0f}s)')
    fails = {}
    nfail = 0
    for i, ((k, fpcr, x, sp, nzcv, v, st), (ex, esp, enz, ev, emh)) in enumerate(zip(cases, expect)):
        rec = data[i * OUT_SIZE:(i + 1) * OUT_SIZE]
        if len(rec) < OUT_SIZE:
            break
        hx_ = struct.unpack_from('<31Q', rec, 0)
        hsp, hnz, hmh = struct.unpack_from('<QQQ', rec, 248)
        hv = [int.from_bytes(rec[272 + 16 * j:288 + 16 * j], 'little') for j in range(32)]
        diffs = [f'x{j}: got {hx_[j]:x} want {ex[j]:x}' for j in range(31) if hx_[j] != ex[j]]
        if hsp != esp:
            diffs.append(f'sp: got {hsp:x} want {esp:x}')
        if hnz != enz:
            diffs.append(f'nzcv: got {hnz:x} want {enz:x}')
        if st and hmh != emh:
            diffs.append('memory differs')
        for j in range(32):
            if hv[j] != ev[j]:
                diffs.append(f'v{j}: got {hv[j]:032x} want {ev[j]:032x} (in {v[j]:032x})')
        if diffs:
            nfail += 1
            w = pick[k]
            if w not in fails:
                ins = [f'v{j}={v[j]:032x}' for j in sorted({(w >> 5) & 31, (w >> 16) & 31, w & 31})]
                fails[w] = (fpcr, diffs[:3], ins)
    print(f'{len(pick)} encodings, {len(cases)} cases: {nfail} failing cases, {len(fails)} failing encodings '
          f'({time.time() - t0:.0f}s total)')
    byname = Counter(dis(w).split(' ')[0] for w in fails)
    if fails:
        print('  failing by mnemonic:', dict(byname.most_common(40)))
    for w, (fpcr, d, ins) in list(fails.items())[:args.show]:
        print(f'  {w:08x} {dis(w):40} fpcr={fpcr:x} {"; ".join(d)}')
        print(f'      {" ".join(ins)}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
