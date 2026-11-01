"""AArch64 -> C static recompiler.

Integer, memory, and control flow are translated to C. FP/SIMD instructions (and CRC32) are
either re-emitted as inline assembly (simd='asm', AArch64 hosts), with guest V registers
loaded from / stored to the Cpu struct around each batch, so their semantics are
bit-exact by construction, or lowered to portable C (simd='c', tools/simd_c.py + runtime/simd.h).
"""
import struct

# Cpu struct offsets (must match runtime/cpu.h)
OFF_X = 0
OFF_SP = 256
OFF_PC = 264
OFF_NZCV = 272
OFF_V = 512

COND_NAMES = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al', 'nv']
SCRATCH = [9, 10, 11, 12, 13, 14, 15, 16, 17]


class Unsupported(Exception):
    pass


def bits(w, lo, n):
    return (w >> lo) & ((1 << n) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def ones(n):
    return (1 << n) - 1


def ror(v, r, size):
    r %= size
    return ((v >> r) | (v << (size - r))) & ones(size) if r else v


def replicate(v, esize, size):
    out = 0
    for i in range(0, size, esize):
        out |= v << i
    return out


def decode_bit_masks(N, imms, immr, size, immediate):
    x = (N << 6) | (~imms & 0x3F)
    if x == 0:
        raise Unsupported('reserved bitmask')
    length = x.bit_length() - 1
    if length < 1:
        raise Unsupported('reserved bitmask')
    levels = ones(length)
    if immediate and (imms & levels) == levels:
        raise Unsupported('reserved bitmask')
    S = imms & levels
    R = immr & levels
    diff = (S - R) & 0x3F
    esize = 1 << length
    d = diff & ones(length)
    welem = ones(S + 1)
    telem = ones(d + 1)
    wmask = replicate(ror(welem, R, esize), esize, size)
    tmask = replicate(telem, esize, size)
    return wmask, tmask


def hx(v):
    return f'0x{v:x}ull'


# ---------------------------------------------------------------- register helpers

def X(r):
    return '0ull' if r == 31 else f'c->x[{r}]'


def XS(r):
    return 'c->sp' if r == 31 else f'c->x[{r}]'


def W(r):
    return '0u' if r == 31 else f'(uint32_t)c->x[{r}]'


def WS(r):
    return '(uint32_t)c->sp' if r == 31 else f'(uint32_t)c->x[{r}]'


def R(sf, r, sp=False):
    if sf:
        return XS(r) if sp else X(r)
    return WS(r) if sp else W(r)


def set_reg(sf, r, expr, sp=False):
    """Statement writing expr to Rd (zero-extending 32-bit results)."""
    if r == 31 and not sp:
        return f'(void)({expr});'
    dst = 'c->sp' if r == 31 else f'c->x[{r}]'
    if sf:
        return f'{dst} = (uint64_t)({expr});'
    return f'{dst} = (uint32_t)({expr});'


def shift_expr(sf, val, typ, amt):
    if amt == 0 and typ != 3:
        return val
    if typ == 0:
        return f'({val} << {amt})'
    if typ == 1:
        return f'({val} >> {amt})'
    if typ == 2:
        return f'(uint64_t)((int64_t){val} >> {amt})' if sf else f'(uint32_t)((int32_t){val} >> {amt})'
    return f'ROR{64 if sf else 32}({val}, {amt})'


def extend_expr(r, option, shift):
    base = X(r)
    e = {
        0: f'(uint64_t)(uint8_t){base}',
        1: f'(uint64_t)(uint16_t){base}',
        2: f'(uint64_t)(uint32_t){base}',
        3: base,
        4: f'(uint64_t)(int64_t)(int8_t){base}',
        5: f'(uint64_t)(int64_t)(int16_t){base}',
        6: f'(uint64_t)(int64_t)(int32_t){base}',
        7: base,
    }[option]
    return f'({e} << {shift})' if shift else e


# ---------------------------------------------------------------- instruction classes

class Insn:
    """Result of lifting one instruction."""
    __slots__ = ('kind', 'code', 'target', 'cond', 'asm')

    def __init__(self, kind, code='', target=None, cond=None, asm=None):
        self.kind = kind      # 'op', 'asm', 'b', 'bcond', 'bl', 'blr', 'br', 'ret', 'trap'
        self.code = code      # C statements (for 'op'), or condition expr for 'bcond'
        self.target = target  # branch target (int) or register expr
        self.cond = cond
        self.asm = asm        # AsmInsn for 'asm'


class AsmInsn:
    """An FP/SIMD instruction to pass through as inline asm."""
    __slots__ = ('word', 'vregs', 'gprs', 'flags', 'consts')

    def __init__(self, word, vregs, gprs, flags=False, consts=None):
        self.word = word
        self.vregs = vregs    # set of V register numbers touched
        self.gprs = gprs      # list of (bit position, guest reg or 'sp')
        self.flags = flags
        self.consts = consts or []  # list of (bit position, constant) loaded into scratch


def is_fp_simd_dp(w):
    return bits(w, 25, 3) == 0b111


def is_ldst(w):
    return bits(w, 25, 1) == 0 and bits(w, 27, 1) == 1


class Lifter:
    def __init__(self, base, simd='asm'):
        self.base = base  # absolute load address of the module (guest == host)
        if simd not in ('asm', 'c'):
            raise ValueError(f'simd={simd!r}')
        self.sc = None
        if simd == 'c':
            from simd_c import SimdC
            self.sc = SimdC()

    def lift(self, w, pc):
        """pc is absolute. Returns an Insn."""
        op0 = bits(w, 25, 4)
        if (op0 & 0b1110) == 0b1000:
            return self.dp_imm(w, pc)
        if (op0 & 0b1110) == 0b1010:
            return self.branch_sys(w, pc)
        if (op0 & 0b0101) == 0b0100:
            return self.ldst(w, pc)
        if (op0 & 0b0111) == 0b0101:
            return self.dp_reg(w, pc)
        if (op0 & 0b0111) == 0b0111:
            return self.fp_simd(w, pc)
        # padding, trap words (e7ffdefe), header data: trap if ever executed
        return Insn('trap', f'rt_udf(c, {hx(pc)});')

    # ------------------------------------------------------------ data processing (imm)

    def dp_imm(self, w, pc):
        op = bits(w, 23, 3)
        sf = bits(w, 31, 1)
        rd, rn = bits(w, 0, 5), bits(w, 5, 5)
        if op in (0, 1):  # PC-relative
            imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21)
            if bits(w, 31, 1):
                val = (pc & ~0xFFF) + (imm << 12)
            else:
                val = pc + imm
            return Insn('op', set_reg(1, rd, hx(val & ones(64))))
        if op == 2:  # add/sub imm
            opc, S, sh = bits(w, 30, 1), bits(w, 29, 1), bits(w, 22, 1)
            imm = bits(w, 10, 12) << (12 if sh else 0)
            a = R(sf, rn, sp=True)
            if S:
                fn = ('subs' if opc else 'adds') + ('64' if sf else '32')
                return Insn('op', set_reg(sf, rd, f'{fn}(c, {a}, {hx(imm)})'))
            return Insn('op', set_reg(sf, rd, f'{a} {"-" if opc else "+"} {hx(imm)}', sp=True))
        if op == 4:  # logical imm
            opc, N = bits(w, 29, 2), bits(w, 22, 1)
            if not sf and N:
                raise Unsupported('logical imm N=1 32-bit')
            size = 64 if sf else 32
            imm, _ = decode_bit_masks(N, bits(w, 10, 6), bits(w, 16, 6), size, True)
            a = R(sf, rn)
            if opc == 0:
                return Insn('op', set_reg(sf, rd, f'{a} & {hx(imm)}', sp=True))
            if opc == 1:
                return Insn('op', set_reg(sf, rd, f'{a} | {hx(imm)}', sp=True))
            if opc == 2:
                return Insn('op', set_reg(sf, rd, f'{a} ^ {hx(imm)}', sp=True))
            return Insn('op', set_reg(sf, rd, f'logic{size}(c, {a} & {hx(imm)})'))
        if op == 5:  # move wide
            opc, hw = bits(w, 29, 2), bits(w, 21, 2)
            if not sf and hw > 1:
                raise Unsupported('movw hw')
            imm = bits(w, 5, 16) << (hw * 16)
            if opc == 0:
                return Insn('op', set_reg(sf, rd, hx(~imm & ones(64 if sf else 32))))
            if opc == 2:
                return Insn('op', set_reg(sf, rd, hx(imm)))
            if opc == 3:
                mask = ~(0xFFFF << (hw * 16)) & ones(64)
                return Insn('op', set_reg(sf, rd, f'({X(rd)} & {hx(mask)}) | {hx(imm)}'))
            raise Unsupported('movw opc')
        if op == 6:  # bitfield
            opc, N = bits(w, 29, 2), bits(w, 22, 1)
            immr, imms = bits(w, 16, 6), bits(w, 10, 6)
            size = 64 if sf else 32
            if N != sf:
                raise Unsupported('bitfield N')
            wmask, tmask = decode_bit_masks(N, imms, immr, size, False)
            src = R(sf, rn)
            rot = f'ROR{size}({src}, {immr})' if immr else src
            if opc == 2:  # UBFM
                return Insn('op', set_reg(sf, rd, f'{rot} & {hx(wmask & tmask)}'))
            if opc == 0:  # SBFM
                top = f'(uint{size}_t)(-(int{size}_t)(({src} >> {imms}) & 1))'
                return Insn('op', set_reg(
                    sf, rd, f'({top} & {hx(~tmask & ones(size))}) | ({rot} & {hx(wmask & tmask)})'))
            if opc == 1:  # BFM
                dst = R(sf, rd)
                bot = f'(({dst} & {hx(~wmask & ones(size))}) | ({rot} & {hx(wmask)}))'
                return Insn('op', set_reg(
                    sf, rd, f'({dst} & {hx(~tmask & ones(size))}) | ({bot} & {hx(tmask)})'))
            raise Unsupported('bitfield opc')
        if op == 7:  # EXTR
            rm, lsb = bits(w, 16, 5), bits(w, 10, 6)
            size = 64 if sf else 32
            if lsb == 0:
                return Insn('op', set_reg(sf, rd, R(sf, rm)))
            return Insn('op', set_reg(
                sf, rd, f'({R(sf, rm)} >> {lsb}) | ((uint{size}_t){R(sf, rn)} << {size - lsb})'))
        raise Unsupported(f'dp_imm {w:08x}')

    # ------------------------------------------------------------ branches / system

    def branch_sys(self, w, pc):
        if bits(w, 26, 5) == 0b00101:  # B / BL
            t = (pc + sext(bits(w, 0, 26), 26) * 4) & ones(64)
            return Insn('bl' if bits(w, 31, 1) else 'b', target=t)
        if bits(w, 25, 7) == 0b0101010 and not bits(w, 4, 1):  # B.cond
            t = pc + sext(bits(w, 5, 19), 19) * 4
            cond = bits(w, 0, 4)
            if cond >= 14:
                return Insn('b', target=t)
            return Insn('bcond', target=t, cond=f'COND_{COND_NAMES[cond].upper()}(c)')
        if bits(w, 25, 6) == 0b011010:  # CBZ/CBNZ
            sf, nz, rt = bits(w, 31, 1), bits(w, 24, 1), bits(w, 0, 5)
            t = pc + sext(bits(w, 5, 19), 19) * 4
            return Insn('bcond', target=t, cond=f'{R(sf, rt)} {"!=" if nz else "=="} 0')
        if bits(w, 25, 6) == 0b011011:  # TBZ/TBNZ
            b = (bits(w, 31, 1) << 5) | bits(w, 19, 5)
            nz, rt = bits(w, 24, 1), bits(w, 0, 5)
            t = pc + sext(bits(w, 5, 14), 14) * 4
            return Insn('bcond', target=t, cond=f'(({X(rt)} >> {b}) & 1) {"!=" if nz else "=="} 0')
        if bits(w, 25, 7) == 0b1101011:  # branch register
            opc, rn = bits(w, 21, 4), bits(w, 5, 5)
            if bits(w, 16, 5) != 31 or bits(w, 10, 6) or bits(w, 0, 5):
                raise Unsupported('branch reg variant')
            if opc == 0:
                return Insn('br', target=X(rn))
            if opc == 1:
                return Insn('blr', target=X(rn))
            if opc == 2:
                return Insn('ret')
            raise Unsupported('branch reg opc')
        if bits(w, 24, 8) == 0b11010100:  # exception generation
            opc, ll, imm = bits(w, 21, 3), bits(w, 0, 5), bits(w, 5, 16)
            if opc == 0 and ll == 1:
                return Insn('op', f'rt_svc(c, {imm}, {hx(pc)});')
            if opc == 1 and ll == 0:
                return Insn('trap', f'rt_brk(c, {imm}, {hx(pc)});')
            raise Unsupported('exception')
        if bits(w, 22, 10) == 0b1101010100:
            return self.system(w, pc)
        raise Unsupported(f'branch_sys {w:08x}')

    def system(self, w, pc):
        L = bits(w, 21, 1)
        op0, op1, crn, crm, op2, rt = (bits(w, 19, 2), bits(w, 16, 3), bits(w, 12, 4),
                                       bits(w, 8, 4), bits(w, 5, 3), bits(w, 0, 5))
        if op0 == 0 and not L:
            if crn == 2 and op1 == 3 and rt == 31:  # hints
                return Insn('op', '/* hint */')
            if crn == 3 and op1 == 3 and rt == 31:  # barriers
                if op2 == 2:
                    return Insn('op', 'c->excl_addr = ~0ull;')
                return Insn('op', '__atomic_thread_fence(__ATOMIC_SEQ_CST);')
            raise Unsupported(f'pstate/system {w:08x}')
        if op0 == 1:  # SYS
            if not L and op1 == 3 and crn == 7 and crm == 4 and op2 == 1:  # DC ZVA
                return Insn('op', f'memset((void*)(uintptr_t)({X(rt)} & ~63ull), 0, 64);')
            return Insn('op', '/* cache maintenance */')
        key = (op0, op1, crn, crm, op2)
        if L:  # MRS
            src = {
                (3, 3, 13, 0, 3): 'c->tpidrro',
                (3, 3, 13, 0, 2): 'c->tpidr',
                (3, 3, 14, 0, 0): '19200000ull',
                (3, 3, 14, 0, 1): 'rt_ticks()',
                (3, 3, 14, 0, 2): 'rt_ticks()',
                (3, 3, 4, 2, 0): 'c->nzcv',
                (3, 3, 4, 4, 0): 'c->fpcr' if self.sc else 'rt_get_fpcr()',
                (3, 3, 4, 4, 1): 'c->fpsr' if self.sc else 'rt_get_fpsr()',
                (3, 3, 0, 0, 7): '4ull',
                (3, 3, 0, 0, 1): '0x8444c004ull',
            }.get(key)
            if src is None:
                raise Unsupported(f'mrs {key}')
            return Insn('op', set_reg(1, rt, src))
        dst = {
            (3, 3, 13, 0, 2): 'c->tpidr = {v};',
            (3, 3, 4, 2, 0): 'c->nzcv = {v} & 0xf0000000ull;',
            (3, 3, 4, 4, 0): 'rt_fpcr_write(c, {v});' if self.sc else 'rt_set_fpcr({v});',
            (3, 3, 4, 4, 1): 'c->fpsr = {v} & 0xf800009full;' if self.sc else 'rt_set_fpsr({v});',
        }.get(key)
        if dst is None:
            raise Unsupported(f'msr {key}')
        return Insn('op', dst.format(v=X(rt)))

    # ------------------------------------------------------------ loads / stores

    def ldst(self, w, pc):
        V = bits(w, 26, 1)
        if V:
            if self.sc:
                return Insn('op', self.sc.ldst(w, pc))
            return self.ldst_simd(w, pc)
        if bits(w, 24, 6) == 0b001000:
            return self.ldst_excl(w, pc)
        if bits(w, 27, 3) == 0b011 and bits(w, 24, 2) == 0:  # literal
            opc, rt = bits(w, 30, 2), bits(w, 0, 5)
            addr = pc + sext(bits(w, 5, 19), 19) * 4
            if opc == 0:
                return Insn('op', set_reg(0, rt, f'LD32({hx(addr)})'))
            if opc == 1:
                return Insn('op', set_reg(1, rt, f'LD64({hx(addr)})'))
            if opc == 2:
                return Insn('op', set_reg(1, rt, f'(int64_t)(int32_t)LD32({hx(addr)})'))
            return Insn('op', '/* prfm */')
        if bits(w, 27, 3) == 0b101:
            return self.ldst_pair(w)
        if bits(w, 27, 3) == 0b111:
            return self.ldst_reg(w)
        raise Unsupported(f'ldst {w:08x}')

    def ldst_excl(self, w, pc):
        size, o2, L, o1, rs, o0, rn, rt = (bits(w, 30, 2), bits(w, 23, 1), bits(w, 22, 1), bits(w, 21, 1),
                                           bits(w, 16, 5), bits(w, 15, 1), bits(w, 5, 5), bits(w, 0, 5))
        nbits = 8 << size
        addr = XS(rn)
        if o1:
            raise Unsupported('exclusive pair / CAS')
        if o2 == 0:
            if L:
                return Insn('op', set_reg(1, rt, f'ldx{nbits}(c, {addr})'))
            return Insn('op', f'{{ uint32_t st = stx{nbits}(c, {addr}, {X(rt)}); {set_reg(0, rs, "st")} }}')
        if L:
            return Insn('op', set_reg(1, rt, f'lda{nbits}({addr})'))
        return Insn('op', f'stl{nbits}({addr}, {X(rt)});')

    def ldst_pair(self, w):
        opc, mode, L = bits(w, 30, 2), bits(w, 23, 2), bits(w, 22, 1)
        imm7, rt2, rn, rt = sext(bits(w, 15, 7), 7), bits(w, 10, 5), bits(w, 5, 5), bits(w, 0, 5)
        if opc == 3:
            raise Unsupported('pair opc 3')
        scale = 3 if opc == 2 else 2
        off = imm7 << scale
        base = XS(rn)
        post = mode == 1
        wb = mode in (1, 3)
        ea = base if post else (f'{base} + {off}' if off >= 0 else f'{base} - {-off}')
        lines = [f'uint64_t ea = {ea};']
        if L:
            if opc == 2:
                lines.append('uint64_t a = LD64(ea), b = LD64(ea + 8);')
            elif opc == 1:  # LDPSW
                lines.append('uint64_t a = (int64_t)(int32_t)LD32(ea), b = (int64_t)(int32_t)LD32(ea + 4);')
            else:
                lines.append('uint64_t a = LD32(ea), b = LD32(ea + 4);')
        else:
            if opc == 2:
                lines.append(f'ST64(ea, {X(rt)}); ST64(ea + 8, {X(rt2)});')
            else:
                lines.append(f'ST32(ea, {W(rt)}); ST32(ea + 4, {W(rt2)});')
        if wb:
            dst = 'c->sp' if rn == 31 else f'c->x[{rn}]'
            if not post:
                lines.append(f'{dst} = ea;')
            else:
                lines.append(f'{dst} = ea + {off};' if off >= 0 else f'{dst} = ea - {-off};')
        if L:
            lines.append(set_reg(1, rt, 'a'))
            lines.append(set_reg(1, rt2, 'b'))
        return Insn('op', '{ ' + ' '.join(lines) + ' }')

    def ldst_reg(self, w):
        size, opc, rn, rt = bits(w, 30, 2), bits(w, 22, 2), bits(w, 5, 5), bits(w, 0, 5)
        base = XS(rn)
        wb = None
        if bits(w, 24, 2) == 1:
            ea = f'{base} + {hx(bits(w, 10, 12) << size)}'
        else:
            kind = bits(w, 10, 2)
            if bits(w, 21, 1):
                if kind != 2:
                    raise Unsupported('ldst reg atomic/pac')
                rm, option, S = bits(w, 16, 5), bits(w, 13, 3), bits(w, 12, 1)
                if option not in (2, 3, 6, 7):
                    raise Unsupported('ldst reg option')
                ea = f'{base} + {extend_expr(rm, option, size if S else 0)}'
            else:
                imm9 = sext(bits(w, 12, 9), 9)
                sign_imm = f'+ {imm9}' if imm9 >= 0 else f'- {-imm9}'
                if kind in (0, 2):  # unscaled, unprivileged
                    ea = f'{base} {sign_imm}'
                elif kind == 1:  # post
                    ea = base
                    wb = f'{base} {sign_imm}'
                else:  # pre
                    ea = f'{base} {sign_imm}'
                    wb = 'ea'
        nbits = 8 << size
        if opc == 0:
            parts = [f'ST{nbits}(ea, {X(rt) if size == 3 else W(rt)});']
            load = None
        elif opc == 1:
            parts = [f'uint64_t v = LD{nbits}(ea);']
            load = set_reg(1, rt, 'v')
        elif size == 3:
            return Insn('op', '/* prfm */')
        elif size == 2 and opc == 3:
            raise Unsupported('ldst size2 opc3')
        else:
            parts = [f'uint64_t v = (uint64_t)(int64_t)(int{nbits}_t)LD{nbits}(ea);']
            load = set_reg(1 if opc == 2 else 0, rt, 'v')
        parts.insert(0, f'uint64_t ea = {ea};')
        if wb:
            # writeback before the register load, so a load into rn wins
            parts.append(f'{"c->sp" if rn == 31 else f"c->x[{rn}]"} = {wb};')
        if load:
            parts.append(load)
        return Insn('op', '{ ' + ' '.join(parts) + ' }')

    def ldst_simd(self, w, pc):
        """FP/SIMD loads and stores are passed through as asm; GPR address regs are remapped."""
        rt = bits(w, 0, 5)
        if bits(w, 27, 3) == 0b011 and bits(w, 24, 2) == 0:  # literal -> LDR (imm, offset 0) via scratch
            opc = bits(w, 30, 2)
            size, o = {0: (2, 1), 1: (3, 1), 2: (0, 3)}[opc]
            addr = pc + sext(bits(w, 5, 19), 19) * 4
            new = (size << 30) | 0x3D000000 | (o << 22) | (31 << 5) | rt
            return Insn('asm', asm=AsmInsn(new, {rt}, [], consts=[(5, addr)]))
        if bits(w, 31, 1) == 0 and bits(w, 24, 6) in (0b001100, 0b001101):  # structures
            post = bits(w, 23, 1)
            rn, rm = bits(w, 5, 5), bits(w, 16, 5)
            if bits(w, 24, 1) == 0:
                n = {0: 4, 2: 4, 4: 3, 6: 3, 7: 1, 8: 2, 10: 2}.get(bits(w, 12, 4))
                if n is None:
                    raise Unsupported('ldst multiple opcode')
            else:
                n = ((bits(w, 13, 1) << 1) | bits(w, 21, 1)) + 1
            vregs = {(rt + i) % 32 for i in range(n)}
            gprs = [(5, 'sp' if rn == 31 else rn)]
            if post and rm != 31:
                gprs.append((16, rm))
            return Insn('asm', asm=AsmInsn(w, vregs, gprs))
        rn = bits(w, 5, 5)
        gprs = [(5, 'sp' if rn == 31 else rn)]
        vregs = {rt}
        if bits(w, 27, 3) == 0b101:  # pair
            vregs.add(bits(w, 10, 5))
        elif bits(w, 27, 3) == 0b111 and bits(w, 24, 2) == 0 and bits(w, 21, 1) and bits(w, 10, 2) == 2:
            rm = bits(w, 16, 5)
            if rm != 31:
                gprs.append((16, rm))
        return Insn('asm', asm=AsmInsn(w, vregs, gprs))

    # ------------------------------------------------------------ data processing (reg)

    def dp_reg(self, w, pc):
        sf = bits(w, 31, 1)
        rd, rn, rm = bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5)
        size = 64 if sf else 32
        op1 = bits(w, 28, 1)
        if not op1:
            if bits(w, 24, 4) == 0b1010:  # logical shifted
                opc, sh, N, amt = bits(w, 29, 2), bits(w, 22, 2), bits(w, 21, 1), bits(w, 10, 6)
                if not sf and amt >= 32:
                    raise Unsupported('logical shift amount')
                b = shift_expr(sf, R(sf, rm), sh, amt)
                if N:
                    b = f'(uint{size}_t)~{b}'
                a = R(sf, rn)
                expr = {0: f'{a} & {b}', 1: f'{a} | {b}', 2: f'{a} ^ {b}', 3: f'{a} & {b}'}[opc]
                if opc == 3:
                    return Insn('op', set_reg(sf, rd, f'logic{size}(c, (uint{size}_t)({expr}))'))
                return Insn('op', set_reg(sf, rd, expr))
            if bits(w, 24, 4) == 0b1011:  # add/sub shifted or extended
                op, S = bits(w, 30, 1), bits(w, 29, 1)
                if bits(w, 21, 1) == 0:
                    sh, amt = bits(w, 22, 2), bits(w, 10, 6)
                    if sh == 3:
                        raise Unsupported('add shift ror')
                    a = R(sf, rn)
                    b = shift_expr(sf, R(sf, rm), sh, amt)
                    sp_ok = False
                else:
                    if bits(w, 22, 2):
                        raise Unsupported('add ext opt')
                    option, amt = bits(w, 13, 3), bits(w, 10, 3)
                    a = R(sf, rn, sp=True)
                    b = extend_expr(rm, option, amt)
                    if not sf:
                        b = f'(uint32_t)({b})'
                    sp_ok = not S
                if S:
                    fn = ('subs' if op else 'adds') + str(size)
                    return Insn('op', set_reg(sf, rd, f'{fn}(c, {a}, (uint{size}_t)({b}))'))
                return Insn('op', set_reg(sf, rd, f'{a} {"-" if op else "+"} {b}', sp=sp_ok))
            raise Unsupported(f'dp_reg {w:08x}')
        op2 = bits(w, 21, 4)
        if bits(w, 24, 4) == 0b1011:  # 3-source
            op54, op31, o0, ra = bits(w, 29, 2), bits(w, 21, 3), bits(w, 15, 1), bits(w, 10, 5)
            if op54:
                raise Unsupported('dp3 op54')
            sgn = '-' if o0 else '+'
            if op31 == 0:
                return Insn('op', set_reg(sf, rd, f'{R(sf, ra)} {sgn} {R(sf, rn)} * {R(sf, rm)}'))
            if op31 == 1:
                return Insn('op', set_reg(1, rd, f'{X(ra)} {sgn} (uint64_t)((int64_t)(int32_t){W(rn)} * (int64_t)(int32_t){W(rm)})'))
            if op31 == 5:
                return Insn('op', set_reg(1, rd, f'{X(ra)} {sgn} (uint64_t){W(rn)} * (uint64_t){W(rm)}'))
            if op31 == 2:
                return Insn('op', set_reg(
                    1, rd, f'(uint64_t)(((__int128)(int64_t){X(rn)} * (__int128)(int64_t){X(rm)}) >> 64)'))
            if op31 == 6:
                return Insn('op', set_reg(1, rd, f'(uint64_t)(((unsigned __int128){X(rn)} * {X(rm)}) >> 64)'))
            raise Unsupported('dp3')
        if op2 == 0:  # add/sub with carry
            op, S = bits(w, 30, 1), bits(w, 29, 1)
            fn = ('sbc' if op else 'adc') + ('s' if S else '') + str(size)
            return Insn('op', set_reg(sf, rd, f'{fn}(c, {R(sf, rn)}, {R(sf, rm)})'))
        if op2 == 2:  # conditional compare
            op, imm_form, cond, nzcv = bits(w, 30, 1), bits(w, 11, 1), bits(w, 12, 4), bits(w, 0, 4)
            b = hx(rm) if imm_form else R(sf, rm)
            fn = ('subs' if op else 'adds') + str(size)
            return Insn('op', f'if ({cond_expr(cond)}) (void){fn}(c, {R(sf, rn)}, {b}); '
                              f'else c->nzcv = {hx(nzcv << 28)};')
        if op2 == 4:  # conditional select
            op, o2, cond = bits(w, 30, 1), bits(w, 10, 2), bits(w, 12, 4)
            b = R(sf, rm)
            alt = {(0, 0): b, (0, 1): f'{b} + 1', (1, 0): f'~{b}', (1, 1): f'-{b}'}.get((op, o2))
            if alt is None:
                raise Unsupported('csel variant')
            return Insn('op', set_reg(sf, rd, f'({cond_expr(cond)}) ? {R(sf, rn)} : (uint{size}_t)({alt})'))
        if op2 == 6:
            opcode = bits(w, 10, 6)
            if bits(w, 30, 1):  # 1-source
                if bits(w, 16, 5):
                    raise Unsupported('dp1 opcode2')
                a = R(sf, rn)
                expr = {
                    0: f'rbit{size}({a})',
                    1: f'rev16_{size}({a})',
                    2: f'__builtin_bswap32({a})' if not sf else f'rev32_64({a})',
                    3: f'__builtin_bswap64({a})' if sf else None,
                    4: f'clz{size}({a})',
                    5: f'cls{size}({a})',
                }.get(opcode)
                if expr is None:
                    raise Unsupported('dp1')
                return Insn('op', set_reg(sf, rd, expr))
            if 16 <= opcode <= 23:  # CRC32: host has the same instruction
                if self.sc:
                    return Insn('op', self.sc.crc(w))
                gprs = [(p, r) for p, r in ((0, rd), (5, rn), (16, rm)) if r != 31]
                return Insn('asm', asm=AsmInsn(w, set(), gprs))
            a, b = R(sf, rn), R(sf, rm)
            expr = {
                2: f'udiv{size}({a}, {b})',
                3: f'sdiv{size}({a}, {b})',
                8: f'{a} << ({b} & {size - 1})',
                9: f'{a} >> ({b} & {size - 1})',
                10: f'(uint{size}_t)((int{size}_t){a} >> ({b} & {size - 1}))',
                11: f'ROR{size}({a}, {b} & {size - 1})',
            }.get(opcode)
            if expr is None:
                raise Unsupported('dp2')
            return Insn('op', set_reg(sf, rd, expr))
        raise Unsupported(f'dp_reg {w:08x}')

    # ------------------------------------------------------------ FP / SIMD data processing

    def fp_simd(self, w, pc):
        if self.sc:
            return Insn('op', self.sc.dp(w, pc))
        rd, rn, rm, ra = bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5), bits(w, 10, 5)
        vregs = {rd, rn, rm, ra}
        gprs = []
        flags = False
        if (w & 0x5F20FC00) == 0x1E200000:  # FP <-> integer
            opcode = bits(w, 16, 3)
            if opcode in (0, 1, 4, 5, 6):
                gprs = [(0, rd)]
                vregs = {rn}
            else:
                gprs = [(5, rn)]
                vregs = {rd}
        elif (w & 0x5F200000) == 0x1E000000:  # FP <-> fixed point
            opcode = bits(w, 16, 3)
            if opcode in (0, 1):
                gprs = [(0, rd)]
                vregs = {rn}
            else:
                gprs = [(5, rn)]
                vregs = {rd}
        elif (w & 0x9FE08400) == 0x0E000400 and bits(w, 29, 1) == 0:  # AdvSIMD copy
            imm4 = bits(w, 11, 4)
            if imm4 in (1, 3):
                gprs = [(5, rn)]
                vregs = {rd}
            elif imm4 in (5, 7):
                gprs = [(0, rd)]
                vregs = {rn}
            else:
                vregs = {rd, rn}
        elif (w & 0xBFE08C00) == 0x0E000000:  # TBL/TBX
            n = bits(w, 13, 2) + 1
            vregs = {rd, rm} | {(rn + i) % 32 for i in range(n)}
        if (w & 0x5F203C00) == 0x1E202000 or (w & 0x5F200C00) in (0x1E200400, 0x1E200C00):
            flags = True
        gprs = [(p, r) for p, r in gprs if r != 31]  # register 31 is zr in these encodings
        return Insn('asm', asm=AsmInsn(w, vregs, gprs, flags))


def cond_expr(cond):
    return f'COND_{COND_NAMES[cond].upper()}(c)' if cond < 14 else '1'


def emit_asm_batch(batch):
    """Emit one inline-asm block for a run of FP/SIMD instructions."""
    vregs = set()
    gmap = {}
    consts = []
    words = []
    flags = any(a.flags for a in batch)
    pool = [r for r in SCRATCH if not (flags and r == 16)]  # (x16 holds NZCV around the batch: never a guest reg's)
    for a in batch:
        vregs |= a.vregs
        w = a.word
        for pos, g in a.gprs:
            if g not in gmap:
                if len(gmap) + len(consts) >= len(pool):
                    raise Unsupported('scratch exhausted')
                gmap[g] = pool[len(gmap) + len(consts)]
            w = (w & ~(31 << pos)) | (gmap[g] << pos)
        for pos, val in a.consts:
            if len(gmap) + len(consts) >= len(pool):
                raise Unsupported('scratch exhausted')
            s = pool[len(gmap) + len(consts)]
            consts.append((s, val))
            w = (w & ~(31 << pos)) | (s << pos)
        words.append(w)
    lines = []
    ins = ['"r"(c)']
    for g, s in gmap.items():
        off = OFF_SP if g == 'sp' else OFF_X + 8 * g
        lines.append(f'ldr x{s}, [%0, #{off}]')
    for i, (s, val) in enumerate(consts):
        ins.append(f'"r"({hx(val)})')
        lines.append(f'mov x{s}, %{len(ins) - 1}')
    for v in sorted(vregs):
        lines.append(f'ldr q{v}, [%0, #{OFF_V + 16 * v}]')
    if flags:
        lines.append(f'ldr x16, [%0, #{OFF_NZCV}]')
        lines.append('msr nzcv, x16')
    lines += [f'.inst 0x{x:08x}' for x in words]
    if flags:
        lines.append('mrs x16, nzcv')
        lines.append(f'str x16, [%0, #{OFF_NZCV}]')
    for v in sorted(vregs):
        lines.append(f'str q{v}, [%0, #{OFF_V + 16 * v}]')
    for g, s in gmap.items():
        off = OFF_SP if g == 'sp' else OFF_X + 8 * g
        lines.append(f'str x{s}, [%0, #{off}]')
    clob = [f'"v{v}"' for v in sorted(vregs)]
    clob += [f'"x{s}"' for s in sorted(set(gmap.values()) | {s for s, _ in consts} | ({16} if flags else set()))]
    clob += ['"cc"', '"memory"']
    body = '\\n\\t'.join(lines)
    return f'__asm__ volatile("{body}" :: {", ".join(ins)} : {", ".join(clob)});'
