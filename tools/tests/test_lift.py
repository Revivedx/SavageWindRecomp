"""Differential test: lifted C vs Unicorn, one instruction at a time.

Usage: test_lift.py <nso> <base> [count] [seed]
"""
import hashlib
import os
import random
import struct
import subprocess
import sys

import unicorn as uc
from unicorn import arm64_const as A

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp'))  # (tools/recomp)
from lifter import Lifter, Unsupported, emit_asm_batch, bits
from nso import Nso

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUF = 0x7200000000
BUF_SIZE = 0x40000
BUF_MID = BUF + 0x10000
STATE_FMT = '<32Q Q Q Q 32Q 32Q Q'  # x[32], sp, nzcv, pc, v lo[32], v hi[32], buffer-hash-ish (unused in)
SPECIAL = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFF, 0x7FFFFFFF,
           0x80000000, 0x100000000, 0x3F800000, 0x3FF0000000000000, 0xFF, 0xFFFF, 63, 64, 31, 32]


def rand64(r):
    k = r.random()
    if k < 0.3:
        return r.choice(SPECIAL)
    if k < 0.5:
        return r.getrandbits(8)
    if k < 0.6:
        return struct.unpack('<I', struct.pack('<f', r.uniform(-1000, 1000)))[0] | (r.getrandbits(32) << 32)
    return r.getrandbits(64)


def rand_v(r):
    k = r.random()
    if k < 0.5:
        fs = [r.uniform(-100, 100) for _ in range(4)]
        if r.random() < 0.1:
            fs[r.randrange(4)] = r.choice([0.0, -0.0, float('inf'), float('nan'), 1e-40])
        return int.from_bytes(struct.pack('<4f', *fs), 'little')
    if k < 0.7:
        ds = [r.uniform(-1e6, 1e6) for _ in range(2)]
        return int.from_bytes(struct.pack('<2d', *ds), 'little')
    return r.getrandbits(128)


def is_mem(w):
    return bits(w, 25, 1) == 0 and bits(w, 27, 1) == 1


def classify(w):
    """Skip instructions whose single-step behaviour can't be compared."""
    if (w & 0xFC000000) == 0x94000000 or (w & 0xFC000000) == 0x14000000:
        return 'branch'
    if (w & 0xFFC00000) == 0xD5000000:  # system
        if (w & 0xFFFFF01F) == 0xD503201F or (w & 0xFFFFF0FF) == 0xD503309F or (w & 0xFFFFF0FF) == 0xD50330BF:
            return 'skip'  # hints, barriers
        return 'skip'
    if (w & 0x3F000000) == 0x08000000:  # exclusives
        return 'skip' if bits(w, 23, 1) == 0 else 'ok'
    if (w & 0xFF000000) == 0xD4000000:
        return 'skip'
    if w == 0 or (w & 0xFFFF0000) == 0:
        return 'skip'
    return 'ok'


def to_test_body(ins, pc):
    if ins.kind == 'op':
        return ins.code
    if ins.kind == 'asm':
        return emit_asm_batch([ins.asm])
    nxt = f'0x{pc + 4:x}ull'
    if ins.kind == 'b':
        return f'c->pc = 0x{ins.target:x}ull;'
    if ins.kind == 'bl':
        return f'c->x[30] = {nxt}; c->pc = 0x{ins.target:x}ull;'
    if ins.kind == 'bcond':
        return f'c->pc = ({ins.cond}) ? 0x{ins.target:x}ull : {nxt};'
    if ins.kind == 'br':
        return f'c->pc = {ins.target};'
    if ins.kind == 'blr':
        return f'{{ uint64_t t = {ins.target}; c->x[30] = {nxt}; c->pc = t; }}'
    if ins.kind == 'ret':
        return 'c->pc = c->x[30];'
    raise Unsupported(ins.kind)


def run_unicorn(mu, n, w, pc, state, buf0):
    x, sp, nzcv, v = state
    mu.mem_write(BUF, buf0)
    for i in range(29):
        mu.reg_write(A.UC_ARM64_REG_X0 + i, x[i])
    mu.reg_write(A.UC_ARM64_REG_X29, x[29])
    mu.reg_write(A.UC_ARM64_REG_X30, x[30])
    mu.reg_write(A.UC_ARM64_REG_SP, sp)
    mu.reg_write(A.UC_ARM64_REG_NZCV, nzcv)
    for i in range(32):
        mu.reg_write(A.UC_ARM64_REG_Q0 + i, v[i])
    mu.emu_start(pc, pc + 8, count=1)
    ox = [mu.reg_read(A.UC_ARM64_REG_X0 + i) for i in range(29)]
    ox += [mu.reg_read(A.UC_ARM64_REG_X29), mu.reg_read(A.UC_ARM64_REG_X30)]
    osp = mu.reg_read(A.UC_ARM64_REG_SP)
    onz = mu.reg_read(A.UC_ARM64_REG_NZCV) & 0xF0000000
    ov = [mu.reg_read(A.UC_ARM64_REG_Q0 + i) for i in range(32)]
    opc = mu.reg_read(A.UC_ARM64_REG_PC)
    h = hashlib.md5(bytes(mu.mem_read(BUF, BUF_SIZE))).hexdigest()
    return ox, osp, onz, ov, opc, h


def main():
    path, base = sys.argv[1], int(sys.argv[2], 0)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    r = random.Random(seed)
    n = Nso(path)
    lo, hi = n.text[0], n.text[0] + n.text[1]
    words = struct.unpack(f'<{(hi - lo) // 4}I', bytes(n.image[lo:hi]))

    # Pick unique encodings, spread over the module
    seen, picks = set(), []
    order = list(range(len(words)))
    r.shuffle(order)
    lifter = Lifter(base)
    unsupported = {}
    for i in order:
        w = words[i]
        key = w if not is_mem(w) else w  # every distinct word is a distinct case
        if key in seen or classify(w) == 'skip':
            continue
        seen.add(key)
        pc = base + lo + i * 4
        try:
            ins = lifter.lift(w, pc)
            body = to_test_body(ins, pc)
        except Unsupported as e:
            unsupported[str(e).split(' ')[0]] = unsupported.get(str(e).split(' ')[0], 0) + 1
            continue
        picks.append((w, pc, body))
        if len(picks) >= count:
            break

    work = os.path.join(ROOT, 'build', 'test_lift')
    os.makedirs(work, exist_ok=True)
    img_path = os.path.join(work, 'image.bin')
    with open(img_path, 'wb') as f:
        f.write(n.image)
    with open(os.path.join(work, 'tests.c'), 'w') as f:
        f.write('#include "cpu.h"\n')
        for k, (w, pc, body) in enumerate(picks):
            f.write(f'static void t{k}(Cpu *c) {{ {body} }} /* {w:08x} @ {pc:x} */\n')
        f.write('GuestFn tests[] = {' + ','.join(f't{k}' for k in range(len(picks))) + '};\n')
        f.write(f'int ntests = {len(picks)};\n')
    harness = os.path.join(ROOT, 'tools', 'tests', 'test_harness.c')
    exe = os.path.join(work, 'harness')
    subprocess.check_call(['clang', '-O1', '-Wno-deprecated-declarations', '-fno-strict-aliasing', '-I', os.path.join(ROOT, 'runtime'),
                           '-o', exe, harness, os.path.join(work, 'tests.c')])

    mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
    mu.mem_map(base, len(n.image))
    mu.mem_write(base, bytes(n.image))
    mu.mem_map(BUF, BUF_SIZE)
    # enable FP/SIMD
    mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)

    buf0 = bytes(r.getrandbits(8) for _ in range(4096)) * (BUF_SIZE // 4096)
    with open(os.path.join(work, 'buf.bin'), 'wb') as f:
        f.write(buf0)

    cases, expect = [], []
    for k, (w, pc, body) in enumerate(picks):
        for rep in range(3):
            if is_mem(w) or (w & 0xFFFFF000) == 0xD508B000 or (w & 0xFFFFFFE0) == 0xD50B7420:
                x = [BUF_MID + r.randrange(0, 0x100, 8) for _ in range(31)]
                if bits(w, 21, 1) and bits(w, 10, 2) == 2 or (w & 0xBFA00000) == 0x0C800000:
                    rm = bits(w, 16, 5)
                    if rm != bits(w, 5, 5) and rm < 31:
                        x[rm] = r.randrange(0, 0x100)
                sp = BUF_MID + r.randrange(0, 0x100, 16)
            else:
                x = [rand64(r) for _ in range(31)]
                sp = BUF_MID + r.randrange(0, 0x100, 16)
            nzcv = r.getrandbits(4) << 28
            v = [rand_v(r) for _ in range(32)]
            try:
                res = run_unicorn(mu, n, w, pc, (x, sp, nzcv, v), buf0)
            except uc.UcError as e:
                continue
            cases.append((k, pc, x, sp, nzcv, v))
            expect.append(res)

    with open(os.path.join(work, 'cases.bin'), 'wb') as f:
        for k, pc, x, sp, nzcv, v in cases:
            f.write(struct.pack('<QQ', k, pc))
            f.write(struct.pack('<32Q', *(x + [0])))
            f.write(struct.pack('<QQ', sp, nzcv))
            for q in v:
                f.write(q.to_bytes(16, 'little'))
    out = subprocess.run([exe, img_path, os.path.join(work, 'buf.bin'), os.path.join(work, 'cases.bin'),
                          hex(base)], capture_output=True)
    if out.returncode:
        print('harness failed', out.returncode, out.stderr.decode()[-2000:])
    lines = out.stdout.decode().split('\n')
    fails = {}
    for (k, pc, x, sp, nzcv, v), (ex, esp, enz, ev, epc, eh), line in zip(cases, expect, lines):
        parts = line.split()
        if len(parts) < 37:
            continue
        hx_, hsp, hnz, hpc = [int(p, 16) for p in parts[:31]], int(parts[31], 16), int(parts[32], 16), int(parts[33], 16)
        hh = parts[34]
        hv = [int(p, 16) for p in parts[35:67]]
        diffs = []
        for i in range(31):
            if hx_[i] != ex[i]:
                diffs.append(f'x{i}: got {hx_[i]:x} want {ex[i]:x}')
        if hsp != esp:
            diffs.append(f'sp: got {hsp:x} want {esp:x}')
        if hnz != enz:
            diffs.append(f'nzcv: got {hnz:x} want {enz:x}')
        if hpc != 0 and hpc != epc:
            diffs.append(f'pc: got {hpc:x} want {epc:x}')
        if hh != eh:
            diffs.append('memory differs')
        for i in range(32):
            if hv[i] != ev[i]:
                diffs.append(f'v{i}: got {hv[i]:032x} want {ev[i]:032x}')
        if diffs:
            w = picks[k][0]
            fails.setdefault(w, (pc, diffs[:4]))
    print(f'tested {len(picks)} encodings, {len(cases)} cases, {len(lines) - 1} ran, failing encodings: {len(fails)}')
    print('unsupported:', unsupported)
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for w, (pc, d) in list(fails.items())[:40]:
        dis = next(md.disasm(struct.pack('<I', w), pc), None)
        print(f'  {w:08x} {dis.mnemonic + " " + dis.op_str if dis else "?":40} {"; ".join(d)}')


if __name__ == '__main__':
    main()
