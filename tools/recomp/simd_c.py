"""Portable C lowering of AArch64 FP / AdvSIMD / crypto / CRC32 instructions (lifter --simd=c).

Every encoding is decoded here at lift time and turned into C over the Cpu struct, calling the static
inline helpers in runtime/simd.h with constant fields (sizes, lanes, ops), so clang folds the helpers'
switches. Covers ARMv8.0-A + Crypto + CRC32 (the Switch's Cortex-A57); unallocated or later-architecture
encodings (FP16 arithmetic, RDM, dot product, FCMA, JSCVT, ...) raise Unsupported.
"""
from lifter import COND_NAMES, Unsupported, W, X, XS, bits, extend_expr, hx, set_reg, sext


def lop(name, u=0):
    """C name of a lane op (enum in runtime/simd.h), with the unsigned flag."""
    return f'LOP_{name}' + (' | LOP_U' if u else '')


def vu(sz):
    return ('vu8', 'vu16', 'vu32', 'vu64')[sz]


def vs(sz):
    return ('vs8', 'vs16', 'vs32', 'vs64')[sz]


def ut(sz):
    return ('uint8_t', 'uint16_t', 'uint32_t', 'uint64_t')[sz]


def VR(T, n):
    return f'VREG({T}, {n})'


def VQ(T, Q, n):
    return f'VREG({T}, {n})' if Q else f'VQ({T}, 0, {n})'


def VWr(T, Q, d, e):
    return f'VW({T}, {Q}, {d}, {e});'


def cond(c):
    return f'COND_{COND_NAMES[c].upper()}(c)' if c < 14 else '1'


def gen(op, sz, ne, d, szd, offd, keep, n, szn, offn, m=-1, szm=0, offm=0, bidx=-1, bimm=0):
    return (f'sl_gen(c, {op}, {sz}, {ne}, {d}, {szd}, {offd}, {keep}, {n}, {szn}, {offn}, {m}, {szm}, {offm}, '
            f'{bidx}, {hx(bimm)});')


def same(op, sz, ne, d, n, m=-1, bimm=0, bidx=-1):
    """Lane-wise op, all operands the same size."""
    return gen(op, sz, ne, d, sz, 0, 0, n, sz, 0, m, sz, 0, bidx, bimm)


def lanes(Q, sz):
    return (8 << Q) >> sz


def shuf(T, a, b, idx):
    return f'SHUF({a}, {b}, {", ".join(str(i) for i in idx)})'


def u128(v):
    lo, hi = v & ((1 << 64) - 1), v >> 64
    return f'((u128){hx(hi)} << 64 | {hx(lo)})' if hi else hx(lo)


FT = {0: ('s', 'vf32', 'vu32', 'float', 'VSF32', 'VS32', 'FP_SETS'),
      1: ('d', 'vf64', 'vu64', 'double', 'VSF64', 'VS64', 'FP_SETD')}

FOP3 = {  # vector three-same FP: (U, a, opcode) -> FOP
    (0, 0, 0b11000): 'FOP_MAXNM', (0, 0, 0b11010): 'FOP_ADD', (0, 0, 0b11011): 'FOP_MULX',
    (0, 0, 0b11110): 'FOP_MAX', (0, 0, 0b11111): 'FOP_RECPS',
    (0, 1, 0b11000): 'FOP_MINNM', (0, 1, 0b11010): 'FOP_SUB', (0, 1, 0b11110): 'FOP_MIN', (0, 1, 0b11111): 'FOP_RSQRTS',
    (1, 0, 0b11011): 'FOP_MUL', (1, 0, 0b11111): 'FOP_DIV', (1, 1, 0b11010): 'FOP_ABD',
}
FPAIRV = {(1, 0, 0b11000): 'FOP_MAXNM', (1, 1, 0b11000): 'FOP_MINNM', (1, 0, 0b11010): 'FOP_ADD',
          (1, 0, 0b11110): 'FOP_MAX', (1, 1, 0b11110): 'FOP_MIN'}
FCMP3 = {(0, 0, 0b11100): 0, (1, 0, 0b11100): 1, (1, 1, 0b11100): 2, (1, 0, 0b11101): 3, (1, 1, 0b11101): 4}


def vfp_expand_imm(imm8, N):
    E = 8 if N == 32 else 11
    F = N - E - 1
    sign = imm8 >> 7
    b6 = (imm8 >> 6) & 1
    exp = ((b6 ^ 1) << (E - 1)) | ((((1 << (E - 3)) - 1) if b6 else 0) << 2) | ((imm8 >> 4) & 3)
    frac = (imm8 & 15) << (F - 4)
    return (sign << (N - 1)) | (exp << F) | frac


def simd_expand_imm(op, cmode, imm8):
    c = cmode >> 1
    if c == 0:
        imm = (imm8 << 32) | imm8
    elif c == 1:
        imm = ((imm8 << 8) << 32) | (imm8 << 8)
    elif c == 2:
        imm = ((imm8 << 16) << 32) | (imm8 << 16)
    elif c == 3:
        imm = ((imm8 << 24) << 32) | (imm8 << 24)
    elif c == 4:
        imm = sum(imm8 << s for s in range(0, 64, 16))
    elif c == 5:
        imm = sum((imm8 << 8) << s for s in range(0, 64, 16))
    elif c == 6:
        v = ((imm8 << 8) | 0xFF) if not (cmode & 1) else ((imm8 << 16) | 0xFFFF)
        imm = (v << 32) | v
    else:
        if not (cmode & 1) and not op:
            imm = sum(imm8 << s for s in range(0, 64, 8))
        elif not (cmode & 1) and op:
            imm = 0
            for i in range(8):
                if (imm8 >> i) & 1:
                    imm |= 0xFF << (8 * i)
        elif (cmode & 1) and not op:
            v = vfp_expand_imm(imm8, 32)
            imm = (v << 32) | v
        else:
            imm = vfp_expand_imm(imm8, 64)
    return imm


class SimdC:
    """FP/SIMD decoder emitting portable C. Methods return C statements or raise Unsupported."""

    # ------------------------------------------------------------ top level

    def dp(self, w, pc):
        op0, op1, op2, op3 = bits(w, 28, 4), bits(w, 23, 2), bits(w, 19, 4), bits(w, 10, 9)
        if op0 == 0b0100 and not (op1 & 2) and (op2 & 7) == 5 and (op3 & 0b110000011) == 0b10:
            return self.crypto_aes(w)
        if op0 == 0b0101 and not (op1 & 2) and not (op2 & 4) and (op3 & 0b000100011) == 0:
            return self.crypto_sha3(w)
        if op0 == 0b0101 and not (op1 & 2) and (op2 & 7) == 5 and (op3 & 0b110000011) == 0b10:
            return self.crypto_sha2(w)
        if (op0 & 0b1101) == 0b0101:  # AdvSIMD scalar
            if op1 == 0 and not (op2 & 0b1100) and (op3 & 0b000100001) == 1:
                return self.scalar_copy(w)
            if not (op1 & 2) and (op2 & 7) == 4 and (op3 & 0b110000011) == 0b10:
                return self.misc2(w, scalar=True)
            if not (op1 & 2) and (op2 & 7) == 6 and (op3 & 0b110000011) == 0b10:
                return self.scalar_pairwise(w)
            if not (op1 & 2) and (op2 & 4) and (op3 & 3) == 0:
                return self.three_diff(w, scalar=True)
            if not (op1 & 2) and (op2 & 4) and (op3 & 1):
                return self.three_same(w, scalar=True)
            if op1 == 2 and (op3 & 1):
                return self.shift_imm(w, scalar=True)
            if (op1 & 2) and not (op3 & 1):
                return self.indexed(w, scalar=True)
            raise Unsupported(f'simd scalar {w:08x}')
        if (op0 & 0b1011) == 0 and not (op1 & 2) and not (op2 & 4) and (op3 & 0b000100011) == 0:
            return self.tbl(w)
        if (op0 & 0b1011) == 0 and not (op1 & 2) and not (op2 & 4) and (op3 & 0b000100011) == 2:
            return self.permute(w)
        if (op0 & 0b1011) == 0b0010 and not (op1 & 2) and not (op2 & 4) and (op3 & 0b000100001) == 0:
            return self.ext(w)
        if (op0 & 0b1001) == 0:  # AdvSIMD vector
            if op1 == 0 and not (op2 & 0b1100) and (op3 & 0b000100001) == 1:
                return self.copy(w)
            if not (op1 & 2) and (op2 & 4) and (op3 & 1):
                return self.three_same(w)
            if not (op1 & 2) and (op2 & 4) and (op3 & 3) == 0:
                return self.three_diff(w)
            if not (op1 & 2) and (op2 & 7) == 4 and (op3 & 0b110000011) == 0b10:
                return self.misc2(w)
            if not (op1 & 2) and (op2 & 7) == 6 and (op3 & 0b110000011) == 0b10:
                return self.across(w)
            if op1 == 2 and op2 == 0 and (op3 & 1):
                return self.modimm(w)
            if op1 == 2 and op2 and (op3 & 1):
                return self.shift_imm(w)
            if (op1 & 2) and not (op3 & 1):
                return self.indexed(w)
            raise Unsupported(f'simd vector {w:08x}')
        if (op0 & 0b0101) == 0b0001:  # scalar FP
            if bits(w, 29, 1) or (bits(w, 31, 1) and (op1 & 2 or (op2 & 4 and op3 & 0b111111))):
                raise Unsupported('fp M/S')
            if op1 & 2:
                return self.fp_dp3(w)
            if not (op2 & 4):
                return self.fp_fixed(w)
            if (op3 & 0b111111) == 0:
                return self.fp_int(w)
            if (op3 & 0b11111) == 0b10000:
                return self.fp_dp1(w)
            if (op3 & 0b1111) == 0b1000:
                return self.fp_cmp(w)
            if (op3 & 0b111) == 0b100:
                return self.fp_imm(w)
            if (op3 & 3) == 1:
                return self.fp_ccmp(w)
            if (op3 & 3) == 2:
                return self.fp_dp2(w)
            if (op3 & 3) == 3:
                return self.fp_csel(w)
            raise Unsupported('fp op3')
        raise Unsupported(f'fp/simd {w:08x}')

    # ------------------------------------------------------------ crypto

    def crypto_aes(self, w):
        if bits(w, 22, 2):
            raise Unsupported('aes size')
        opc = bits(w, 12, 5)
        if opc not in (4, 5, 6, 7):
            raise Unsupported('aes opcode')
        return f'sv_aes(c, {opc - 4}, {bits(w, 0, 5)}, {bits(w, 5, 5)});'

    def crypto_sha3(self, w):
        if bits(w, 22, 2):
            raise Unsupported('sha size')
        opc, d, n, m = bits(w, 12, 3), bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5)
        if opc < 3:
            return f'sv_sha1hash(c, {opc}, {d}, {n}, {m});'
        if opc == 3:
            return f'sv_sha_misc(c, 0, {d}, {n}, {m});'
        if opc in (4, 5):
            return f'sv_sha256hash(c, {int(opc == 4)}, {d}, {n}, {m});'
        if opc == 6:
            return f'sv_sha_misc(c, 3, {d}, {n}, {m});'
        raise Unsupported('sha3 opcode')

    def crypto_sha2(self, w):
        if bits(w, 22, 2):
            raise Unsupported('sha size')
        opc, d, n = bits(w, 12, 5), bits(w, 0, 5), bits(w, 5, 5)
        op = {0: 4, 1: 1, 2: 2}.get(opc)
        if op is None:
            raise Unsupported('sha2 opcode')
        return f'sv_sha_misc(c, {op}, {d}, {n}, 0);'

    # ------------------------------------------------------------ copy / permute / table

    @staticmethod
    def _imm5(imm5):
        if not imm5 & 15:
            raise Unsupported('copy imm5')
        sz = (imm5 & -imm5).bit_length() - 1
        return sz, imm5 >> (sz + 1)

    def scalar_copy(self, w):
        if bits(w, 29, 1) or bits(w, 11, 4):
            raise Unsupported('scalar copy')
        sz, idx = self._imm5(bits(w, 16, 5))
        if sz > 3:
            raise Unsupported('scalar dup size')
        d, n = bits(w, 0, 5), bits(w, 5, 5)
        return f'c->v[{d}] = (({("uint8_t", "sa16", "sa32", "sa64")[sz]} *)&c->v[{n}])[{idx}];'

    def copy(self, w):
        Q, op, imm5, imm4 = bits(w, 30, 1), bits(w, 29, 1), bits(w, 16, 5), bits(w, 11, 4)
        d, n = bits(w, 0, 5), bits(w, 5, 5)
        sz, idx = self._imm5(imm5)
        if sz > 3:
            raise Unsupported('copy size')
        P = ('uint8_t', 'sa16', 'sa32', 'sa64')[sz]
        if op:
            if not Q:
                raise Unsupported('ins elem Q')
            idx2 = imm4 >> sz
            return f'(({P} *)&c->v[{d}])[{idx}] = (({P} *)&c->v[{n}])[{idx2}];'
        if imm4 == 0:  # DUP (element)
            if sz == 3 and not Q:
                raise Unsupported('dup 1d')
            nl = 16 >> sz
            return VWr(vu(sz), Q, d, shuf(vu(sz), VR(vu(sz), n), VR(vu(sz), n), [idx] * nl))
        if imm4 == 1:  # DUP (general)
            if sz == 3 and not Q:
                raise Unsupported('dup 1d')
            return VWr(vu(sz), Q, d, f'({vu(sz)}){{}} + ({ut(sz)}){X(n)}')
        if imm4 == 3:  # INS (general)
            if not Q:
                raise Unsupported('ins Q')
            return f'(({P} *)&c->v[{d}])[{idx}] = ({ut(sz)}){X(n)};'
        if imm4 == 5:  # SMOV
            if sz >= 2 + Q:
                raise Unsupported('smov size')
            st = ('int8_t', 'int16_t', 'int32_t')[sz]
            return set_reg(Q, d, f'(uint64_t)(int64_t)({st})(({P} *)&c->v[{n}])[{idx}]')
        if imm4 == 7:  # UMOV
            if (Q and sz != 3) or (not Q and sz == 3):
                raise Unsupported('umov size')
            return set_reg(Q, d, f'(({P} *)&c->v[{n}])[{idx}]')
        raise Unsupported('copy imm4')

    def permute(self, w):
        Q, sz, m, opc, n, d = bits(w, 30, 1), bits(w, 22, 2), bits(w, 16, 5), bits(w, 12, 3), bits(w, 5, 5), bits(w, 0, 5)
        if opc in (0, 4) or (sz == 3 and not Q):
            raise Unsupported('permute')
        ne, NL = lanes(Q, sz), 16 >> sz
        part = opc >> 2
        kind = opc & 3
        idx = []
        for i in range(ne):
            if kind == 1:  # UZP
                j = 2 * i + part
                idx.append(j if j < ne else NL + j - ne)
            elif kind == 2:  # TRN
                p = i & ~1
                idx.append(p + part if not (i & 1) else NL + p + part)
            else:  # ZIP
                base = part * ne // 2
                idx.append(base + i // 2 if not (i & 1) else NL + base + i // 2)
        idx += [0] * (NL - ne)
        return VWr(vu(sz), Q, d, shuf(vu(sz), VR(vu(sz), n), VR(vu(sz), m), idx))

    def ext(self, w):
        Q, m, imm4, n, d = bits(w, 30, 1), bits(w, 16, 5), bits(w, 11, 4), bits(w, 5, 5), bits(w, 0, 5)
        if bits(w, 22, 2) or (not Q and imm4 >= 8):
            raise Unsupported('ext')
        if Q:
            idx = [imm4 + i for i in range(16)]
        else:
            idx = [(imm4 + i) if imm4 + i < 8 else 16 + imm4 + i - 8 for i in range(8)] + [0] * 8
        return VWr('vu8', Q, d, shuf('vu8', VR('vu8', n), VR('vu8', m), idx))

    def tbl(self, w):
        Q, m, ln, op, n, d = bits(w, 30, 1), bits(w, 16, 5), bits(w, 13, 2), bits(w, 12, 1), bits(w, 5, 5), bits(w, 0, 5)
        if bits(w, 22, 2):
            raise Unsupported('tbl op2')
        return f'sv_tbl(c, {Q}, {ln}, {op}, {d}, {n}, {m});'

    # ------------------------------------------------------------ three same

    def three_same(self, w, scalar=False):
        Q, U, sz, m, opc, n, d = (bits(w, 30, 1), bits(w, 29, 1), bits(w, 22, 2), bits(w, 16, 5), bits(w, 11, 5),
                                  bits(w, 5, 5), bits(w, 0, 5))
        if opc >= 0b11000:
            return self.three_same_fp(w, scalar)
        if scalar:
            return self.three_same_scalar(U, sz, opc, d, n, m)
        if sz == 3 and not Q and opc != 3:
            raise Unsupported('3same 1d')
        ne = lanes(Q, sz)
        T, S = vu(sz), vs(sz)
        a, b, o = VR(T, n), VR(T, m), VR(T, d)
        sa, sb = VR(S, n), VR(S, m)
        x, y = (a, b) if U else (sa, sb)
        if opc in (0, 2, 4, 14, 15, 12, 13) and sz == 3:
            raise Unsupported('3same size')
        if opc == 0:
            return same(lop('HADD', U), sz, ne, d, n, m)
        if opc == 1:
            return VWr(T, Q, d, f'({T})__builtin_elementwise_{"add"}_sat({x}, {y})')
        if opc == 2:
            return same(lop('RHADD', U), sz, ne, d, n, m)
        if opc == 3:
            a64, b64, o64 = VR('vu64', n), VR('vu64', m), VR('vu64', d)
            e = [[f'{a64} & {b64}', f'{a64} & ~{b64}', f'{a64} | {b64}', f'{a64} | ~{b64}'],
                 [f'{a64} ^ {b64}', f'({o64} & {a64}) | (~{o64} & {b64})', f'({o64} & ~{b64}) | ({a64} & {b64})',
                  f'({o64} & {b64}) | ({a64} & ~{b64})']][U][sz]
            return VWr('vu64', Q, d, e)
        if opc == 4:
            return same(lop('HSUB', U), sz, ne, d, n, m)
        if opc == 5:
            return VWr(T, Q, d, f'({T})__builtin_elementwise_sub_sat({x}, {y})')
        if opc == 6:
            return VWr(T, Q, d, f'({T})({x} > {y})')
        if opc == 7:
            return VWr(T, Q, d, f'({T})({x} >= {y})')
        if 8 <= opc <= 11:
            return same(lop(('SHL', 'QSHL', 'RSHL', 'QRSHL')[opc - 8], U), sz, ne, d, n, m)
        if opc == 12:
            return VWr(T, Q, d, f'({T})__builtin_elementwise_max({x}, {y})')
        if opc == 13:
            return VWr(T, Q, d, f'({T})__builtin_elementwise_min({x}, {y})')
        if opc in (14, 15):
            e = f'({T})__builtin_elementwise_max({x}, {y}) - ({T})__builtin_elementwise_min({x}, {y})'
            return VWr(T, Q, d, f'{o} + ({e})' if opc == 15 else e)
        if opc == 16:
            return VWr(T, Q, d, f'{a} {"-" if U else "+"} {b}')
        if opc == 17:
            return VWr(T, Q, d, f'({T})({a} == {b})' if U else f'({T})(({a} & {b}) != 0)')
        if sz == 3 and opc != 23:
            raise Unsupported('3same size')
        if opc == 18:
            return VWr(T, Q, d, f'{o} {"-" if U else "+"} {a} * {b}')
        if opc == 19:
            if U:
                if sz:
                    raise Unsupported('pmul size')
                return same(lop('PMUL'), 0, ne, d, n, m)
            return VWr(T, Q, d, f'{a} * {b}')
        if opc in (20, 21, 23):
            if opc == 23 and U:
                raise Unsupported('addp U')
            ev, od = self._pair_idx(Q, sz)
            A, B = (a, b) if (U or opc == 23) else (sa, sb)
            L, R = shuf(T, A, B, ev), shuf(T, A, B, od)
            if opc == 23:
                return VWr(T, Q, d, f'({T})({L} + {R})')
            return VWr(T, Q, d, f'({T})__builtin_elementwise_{"max" if opc == 20 else "min"}({L}, {R})')
        if opc == 22:
            if sz not in (1, 2):
                raise Unsupported('sqdmulh size')
            return same(lop('SQRDMULH' if U else 'SQDMULH'), sz, ne, d, n, m)
        raise Unsupported('3same opcode')

    @staticmethod
    def _pair_idx(Q, sz):
        """Lane indices (into SHUF(a, b)) of the left / right operands of a pairwise op; for Q=0 the
        padding lanes point at a's upper half."""
        ne, NL = lanes(Q, sz), 16 >> sz
        src = list(range(ne)) + [NL + i for i in range(ne)]
        ev = [src[2 * k] for k in range(ne)] + [ne % NL] * (NL - ne)
        od = [src[2 * k + 1] for k in range(ne)] + [ne % NL] * (NL - ne)
        return ev, od

    def three_same_scalar(self, U, sz, opc, d, n, m):
        if opc in (1, 5):
            return same(lop('UQADD' if U else 'SQADD') if opc == 1 else lop('UQSUB' if U else 'SQSUB'), sz, 1, d, n, m)
        if opc in (9, 11):
            return same(lop('QSHL' if opc == 9 else 'QRSHL', U), sz, 1, d, n, m)
        if opc == 22:
            if sz not in (1, 2):
                raise Unsupported('sqdmulh size')
            return same(lop('SQRDMULH' if U else 'SQDMULH'), sz, 1, d, n, m)
        if sz != 3:
            raise Unsupported('scalar 3same size')
        a, b = f'VS64({n})', f'VS64({m})'
        sa, sb = f'(int64_t)VS64({n})', f'(int64_t)VS64({m})'
        x, y = (a, b) if U else (sa, sb)
        if opc == 6:
            return f'c->v[{d}] = ({x} > {y}) ? ~0ull : 0;'
        if opc == 7:
            return f'c->v[{d}] = ({x} >= {y}) ? ~0ull : 0;'
        if opc in (8, 10):
            return same(lop('SHL' if opc == 8 else 'RSHL', U), 3, 1, d, n, m)
        if opc == 16:
            return f'c->v[{d}] = (uint64_t)({a} {"-" if U else "+"} {b});'
        if opc == 17:
            return f'c->v[{d}] = ({a} == {b}) ? ~0ull : 0;' if U else f'c->v[{d}] = ({a} & {b}) ? ~0ull : 0;'
        raise Unsupported('scalar 3same opcode')

    def three_same_fp(self, w, scalar):
        Q, U, a_, sz, m, opc, n, d = (bits(w, 30, 1), bits(w, 29, 1), bits(w, 23, 1), bits(w, 22, 1), bits(w, 16, 5),
                                      bits(w, 11, 5), bits(w, 5, 5), bits(w, 0, 5))
        if not scalar and sz and not Q:
            raise Unsupported('fp 2d Q')
        F, VT, VU, T, SF, SU, SET = FT[sz]
        key = (U, a_, opc)
        if key in FCMP3:
            cop = FCMP3[key]
            if scalar:
                return (f'c->v[{d}] = ({ut(2 + sz)})vcmp_{F}(c, {cop}, ({VT}){{{SF}({n})}}, '
                        f'({VT}){{{SF}({m})}})[0];')
            return VWr(VU, Q, d, f'vcmp_{F}(c, {cop}, {VQ(VT, Q, n)}, {VQ(VT, Q, m)})')
        if scalar:
            if key not in ((0, 0, 0b11011), (0, 0, 0b11111), (0, 1, 0b11111), (1, 1, 0b11010)):
                raise Unsupported('scalar fp 3same')
            fop = FOP3[key]
            x = f'neg_{F}({SF}({n}))' if fop in ('FOP_RECPS', 'FOP_RSQRTS') else f'{SF}({n})'
            return f'{SET}({d}, op_{F}(c, {fop}, {x}, {SF}({m}), 0));'
        A, B = VQ(VT, Q, n), VQ(VT, Q, m)
        if key in FPAIRV:
            ev, od = self._pair_idx(Q, 2 + sz)
            return f'SV_FOP_{F.upper()}({FPAIRV[key]}, {Q}, {d}, {shuf(VT, A, B, ev)}, {shuf(VT, A, B, od)}, ({VT}){{}});'
        if U == 0 and opc == 0b11001:  # FMLA / FMLS: the multiplicand is negated for FMLS
            return f'SV_FOP_{F.upper()}(FOP_MADD, {Q}, {d}, {VQ(VT, Q, d)}, {f"vneg_{F}({A})" if a_ else A}, {B});'
        fop = FOP3.get(key)
        if fop is None:
            raise Unsupported('fp 3same opcode')
        if fop in ('FOP_RECPS', 'FOP_RSQRTS'):
            A = f'vneg_{F}({A})'
        return f'SV_FOP_{F.upper()}({fop}, {Q}, {d}, {A}, {B}, ({VT}){{}});'

    # ------------------------------------------------------------ three different

    def three_diff(self, w, scalar=False):
        Q, U, sz, m, opc, n, d = (bits(w, 30, 1), bits(w, 29, 1), bits(w, 22, 2), bits(w, 16, 5), bits(w, 12, 4),
                                  bits(w, 5, 5), bits(w, 0, 5))
        if scalar:
            if U or opc not in (9, 11, 13) or sz not in (1, 2):
                raise Unsupported('scalar 3diff')
            op = {9: 'SQDMLAL', 11: 'SQDMLSL', 13: 'SQDMULL'}[opc]
            return gen(lop(op), sz, 1, d, sz + 1, 0, 0, n, sz, 0, m, sz, 0)
        if opc == 14:  # PMULL
            if U:
                raise Unsupported('pmull U')
            if sz == 3:
                return f'sl_pmull64(c, {Q}, {d}, {n}, {m});'
            if sz:
                raise Unsupported('pmull size')
        if sz == 3 or opc == 15:
            raise Unsupported('3diff size')
        ne = 8 >> sz
        off = Q * ne
        if opc in (4, 6):  # ADDHN/RADDHN/SUBHN/RSUBHN: narrow into the low or high half
            op = ('ADDHN', 'RADDHN', 'SUBHN', 'RSUBHN')[(opc == 6) * 2 + U]
            return gen(lop(op), sz, ne, d, sz, off, Q, n, sz + 1, 0, m, sz + 1, 0)
        if opc in (1, 3):  # wide
            op = 'ADDW' if opc == 1 else 'SUBW'
            return gen(lop(op, U), sz, ne, d, sz + 1, 0, 0, n, sz + 1, 0, m, sz, off)
        op = {0: 'ADDL', 2: 'SUBL', 5: 'ABAL', 7: 'ABDL', 8: 'MLAL', 10: 'MLSL', 12: 'MULL',
              9: 'SQDMLAL', 11: 'SQDMLSL', 13: 'SQDMULL', 14: 'PMULL'}[opc]
        if op.startswith('SQD'):
            if U or sz not in (1, 2):
                raise Unsupported('sqdmull')
        return gen(lop(op, U if not op.startswith(('SQD', 'PM')) else 0), sz, ne, d, sz + 1, 0, 0, n, sz, off,
                   m, sz, off)

    # ------------------------------------------------------------ two-register misc

    def misc2(self, w, scalar=False):
        Q, U, sz, opc, n, d = bits(w, 30, 1), bits(w, 29, 1), bits(w, 22, 2), bits(w, 12, 5), bits(w, 5, 5), bits(w, 0, 5)
        if opc >= 0b11000 or opc in (0b10110, 0b10111) or 0b01100 <= opc <= 0b01111:
            return self.misc2_fp(w, scalar)
        if scalar:
            return self.misc2_scalar(U, sz, opc, d, n)
        ne = lanes(Q, sz)
        T, S = vu(sz), vs(sz)
        if opc == 0 or (opc == 1 and not U):  # REV64 / REV32 / REV16
            gsz = {(0, 0): 3, (1, 0): 2, (0, 1): 1}.get((U, opc))
            if gsz is None or sz >= gsz:
                raise Unsupported('rev')
            es = 1 << sz
            gb = 1 << gsz
            idx = []
            for i in range(16):
                g, o = i // gb * gb, i % gb
                e, b = o // es, o % es
                idx.append(g + (gb // es - 1 - e) * es + b)
            return VWr('vu8', Q, d, shuf('vu8', VR('vu8', n), VR('vu8', n), idx))
        if sz == 3 and not Q and opc not in (0b10010, 0b10100, 0b10011):
            raise Unsupported('misc 1d')
        if opc in (2, 6):
            if sz == 3:
                raise Unsupported('addlp size')
            return f'sl_addlp(c, {U}, {int(opc == 6)}, {Q}, {sz}, {d}, {n});'
        if opc == 3:
            return same(lop('USQADD' if U else 'SUQADD'), sz, ne, d, n)
        if opc == 4:
            if sz == 3:
                raise Unsupported('clz size')
            return same(lop('CLZ' if U else 'CLS'), sz, ne, d, n)
        if opc == 5:
            if not U:
                if sz:
                    raise Unsupported('cnt size')
                return same(lop('CNT'), 0, ne, d, n)
            if sz == 0:
                return VWr('vu64', Q, d, f'~{VR("vu64", n)}')
            if sz == 1:
                return same(lop('RBIT'), 0, lanes(Q, 0), d, n)
            raise Unsupported('not/rbit size')
        if opc == 7:
            return same(lop('SQNEG' if U else 'SQABS'), sz, ne, d, n)
        a, sa = VR(T, n), VR(S, n)
        if opc == 8:
            return VWr(T, Q, d, f'({T})({sa} {">=" if U else ">"} 0)')
        if opc == 9:
            return VWr(T, Q, d, f'({T})({sa} {"<=" if U else "=="} 0)')
        if opc == 10 and not U:
            return VWr(T, Q, d, f'({T})({sa} < 0)')
        if opc == 11:
            if U:
                return VWr(T, Q, d, f'-{a}')
            return VWr(T, Q, d, f'(({T}){sa} ^ ({T})({sa} >> {(8 << sz) - 1})) - ({T})({sa} >> {(8 << sz) - 1})')
        if opc in (0b10010, 0b10100):  # XTN / SQXTUN / SQXTN / UQXTN
            if sz == 3:
                raise Unsupported('xtn size')
            op = {(0b10010, 0): 'XTN', (0b10010, 1): 'SQXTUN', (0b10100, 0): 'SQXTN', (0b10100, 1): 'UQXTN'}[(opc, U)]
            ne = 8 >> sz
            return gen(lop(op), sz, ne, d, sz, Q * ne, Q, n, sz + 1, 0)
        if opc == 0b10011 and U:  # SHLL
            if sz == 3:
                raise Unsupported('shll size')
            ne = 8 >> sz
            return gen(lop('SHLL'), sz, ne, d, sz + 1, 0, 0, n, sz, Q * ne)
        raise Unsupported('misc2 opcode')

    def misc2_scalar(self, U, sz, opc, d, n):
        if opc == 3:
            return same(lop('USQADD' if U else 'SUQADD'), sz, 1, d, n)
        if opc == 7:
            return same(lop('SQNEG' if U else 'SQABS'), sz, 1, d, n)
        if opc in (0b10010, 0b10100):
            if sz == 3 or (opc == 0b10010 and not U):
                raise Unsupported('scalar xtn')
            op = {(0b10010, 1): 'SQXTUN', (0b10100, 0): 'SQXTN', (0b10100, 1): 'UQXTN'}[(opc, U)]
            return gen(lop(op), sz, 1, d, sz, 0, 0, n, sz + 1, 0)
        if sz != 3:
            raise Unsupported('scalar misc size')
        a, sa = f'VS64({n})', f'(int64_t)VS64({n})'
        if opc == 8:
            return f'c->v[{d}] = ({sa} {">=" if U else ">"} 0) ? ~0ull : 0;'
        if opc == 9:
            return f'c->v[{d}] = ({sa} {"<=" if U else "=="} 0) ? ~0ull : 0;'
        if opc == 10 and not U:
            return f'c->v[{d}] = ({sa} < 0) ? ~0ull : 0;'
        if opc == 11:
            return f'c->v[{d}] = (uint64_t)-{a};' if U else f'c->v[{d}] = ({sa} < 0) ? (uint64_t)-{a} : {a};'
        raise Unsupported('scalar misc opcode')

    def misc2_fp(self, w, scalar):
        """FP two-register misc, vector and scalar."""
        Q, U, a_, sz, opc, n, d = (bits(w, 30, 1), bits(w, 29, 1), bits(w, 23, 1), bits(w, 22, 1), bits(w, 12, 5),
                                   bits(w, 5, 5), bits(w, 0, 5))
        F, VT, VU, T, SF, SU, SET = FT[sz]
        if opc in (0b10110, 0b10111):  # FCVTN / FCVTL / FCVTXN
            if a_:
                raise Unsupported('fcvtn a')
            if opc == 0b10110 and U:
                if not sz:
                    raise Unsupported('fcvtxn size')
                if scalar:
                    return f'FP_SETS({d}, simd_d2s_odd(c, VSF64({n})));'
                return f'sv_fcvtn(c, 1, {Q}, 1, {d}, {n});'
            if scalar or U:
                raise Unsupported('fcvtn/l')
            if opc == 0b10110:
                return f'sv_fcvtn(c, {sz}, {Q}, 0, {d}, {n});'
            return f'sv_fcvtl(c, {sz}, {Q}, {d}, {n});'
        if not scalar and sz and not Q:
            raise Unsupported('fp misc 2d Q')
        x = VQ(VT, Q, n)
        if 0b01100 <= opc <= 0b01111:
            if not a_:
                raise Unsupported('fp16 misc')
            if opc == 0b01111:  # FABS / FNEG
                if scalar:
                    raise Unsupported('scalar fabs')
                mask = '0x80000000u' if not sz else '0x8000000000000000ull'
                return VWr(VU, Q, d, f'{VR(VU, n)} ^ {mask}' if U else f'{VR(VU, n)} & ~{mask}')
            cop = {(0b01100, 0): (2, 0), (0b01100, 1): (1, 0), (0b01101, 0): (0, 0), (0b01101, 1): (1, 1),
                   (0b01110, 0): (2, 1)}.get((opc, U))
            if cop is None:
                raise Unsupported('fcm zero')
            op, swap = cop
            if scalar:
                a, b = f'({VT}){{{SF}({n})}}', f'({VT}){{}}'
                if swap:
                    a, b = b, a
                return f'c->v[{d}] = ({ut(2 + sz)})vcmp_{F}(c, {op}, {a}, {b})[0];'
            a, b = x, f'({VT}){{}}'
            if swap:
                a, b = b, a
            return VWr(VU, Q, d, f'vcmp_{F}(c, {op}, {a}, {b})')
        if opc == 0b11111:
            if not a_:
                raise Unsupported('fp misc 11111')
            if U:
                if scalar:
                    raise Unsupported('scalar fsqrt')
                return f'SV_FOP_{F.upper()}(FOP_SQRT, {Q}, {d}, {x}, {x}, ({VT}){{}});'
            if not scalar:
                raise Unsupported('vector frecpx')
            return f'{SET}({d}, recpx_{F}(c, {SF}({n})));'
        if opc in (0b11000, 0b11001):  # FRINT*
            if scalar:
                raise Unsupported('scalar frint')
            rm = {(0, 0, 0): 0, (0, 0, 1): 2, (0, 1, 0): 1, (0, 1, 1): 3, (1, 0, 0): 4, (1, 0, 1): 5, (1, 1, 1): 6}.get(
                (U, a_, opc & 1))
            if rm is None:
                raise Unsupported('frint')
            return VWr(VT, Q, d, f'vfrint_{F}(c, {x}, {rm})')
        if opc in (0b11010, 0b11011) or (opc == 0b11100 and not a_):  # FCVT[NPMZA][SU]
            rm = {(0b11010, 0): 0, (0b11010, 1): 1, (0b11011, 0): 2, (0b11011, 1): 3, (0b11100, 0): 4}[(opc, a_)]
            if scalar:
                return f'c->v[{d}] = ({ut(2 + sz)})toint_{F}(c, {SF}({n}), {rm}, 0, {32 << sz}, {U});'
            return VWr(VU, Q, d, f'vtoint_{F}(c, {x}, {rm}, 0, {U})')
        if opc == 0b11100:  # URECPE / URSQRTE
            if scalar or sz:
                raise Unsupported('urecpe')
            return same(lop('URSQRTE' if U else 'URECPE'), 2, lanes(Q, 2), d, n)
        if opc == 0b11101:
            if not a_:  # SCVTF / UCVTF
                if scalar:
                    return f'{SET}({d}, fromint_{F}(c, {SU}({n}), {32 << sz}, {U}, 0));'
                return VWr(VT, Q, d, f'vfromint_{F}(c, {VR(VU, n)}, 0, {U})')
            if scalar:  # FRECPE / FRSQRTE
                return f'{SET}({d}, {"rsqrte" if U else "recpe"}_{F}(c, {SF}({n})));'
            return VWr(VT, Q, d, f'v{"rsqrte" if U else "recpe"}_{F}(c, {x})')
        raise Unsupported('fp misc opcode')

    # ------------------------------------------------------------ across lanes / pairwise

    def across(self, w):
        Q, U, sz, opc, n, d = bits(w, 30, 1), bits(w, 29, 1), bits(w, 22, 2), bits(w, 12, 5), bits(w, 5, 5), bits(w, 0, 5)
        if opc in (0b01100, 0b01111):
            if not U:
                raise Unsupported('fp16 across')
            if not Q or (sz & 1):
                raise Unsupported('fp across size')
            fop = {(0b01100, 0): 'FOP_MAXNM', (0b01100, 1): 'FOP_MINNM', (0b01111, 0): 'FOP_MAX',
                   (0b01111, 1): 'FOP_MIN'}[(opc, sz >> 1)]
            return f'sv_fred_s(c, {fop}, {d}, {n});'
        op = {0b00011: 1, 0b01010: 2, 0b11010: 3, 0b11011: 0}.get(opc)
        if op is None or (op == 0 and U) or sz == 3 or (sz == 2 and not Q):
            raise Unsupported('across')
        return f'sl_across(c, {op}, {U}, {Q}, {sz}, {d}, {n});'

    def scalar_pairwise(self, w):
        U, sz, opc, n, d = bits(w, 29, 1), bits(w, 22, 2), bits(w, 12, 5), bits(w, 5, 5), bits(w, 0, 5)
        if not U:
            if opc == 0b11011 and sz == 3:
                return f'c->v[{d}] = (uint64_t)(VS64({n}) + VHI64({n}));'
            raise Unsupported('scalar pairwise')
        fop = {(0b01100, 0): 'FOP_MAXNM', (0b01100, 1): 'FOP_MINNM', (0b01101, 0): 'FOP_ADD',
               (0b01111, 0): 'FOP_MAX', (0b01111, 1): 'FOP_MIN'}.get((opc, sz >> 1))
        if fop is None:
            raise Unsupported('scalar fp pairwise')
        F, VT, VU, T, SF, SU, SET = FT[sz & 1]
        return f'{SET}({d}, pair_{F}(c, {fop}, {VR(VT, n)}[0], {VR(VT, n)}[1]));'

    # ------------------------------------------------------------ modified immediate

    def modimm(self, w):
        Q, op, cmode, o2, d = bits(w, 30, 1), bits(w, 29, 1), bits(w, 12, 4), bits(w, 11, 1), bits(w, 0, 5)
        imm8 = (bits(w, 16, 3) << 5) | bits(w, 5, 5)
        if o2:
            raise Unsupported('fmov half imm')
        if cmode == 15 and op and not Q:
            raise Unsupported('fmov 1d')
        imm = simd_expand_imm(op, cmode, imm8)
        c4 = cmode
        if (c4 < 8 and c4 & 1) or (8 <= c4 < 12 and c4 & 1):  # ORR / BIC
            if op:
                imm = ~imm & ((1 << 64) - 1)
                return VWr('vu64', Q, d, f'{VR("vu64", d)} & {hx(imm)}')
            return VWr('vu64', Q, d, f'{VR("vu64", d)} | {hx(imm)}')
        if op and not (c4 == 14 or c4 == 15):  # MVNI
            imm = ~imm & ((1 << 64) - 1)
        v = imm | (imm << 64) if Q else imm
        return f'c->v[{d}] = {u128(v)};'

    # ------------------------------------------------------------ shift by immediate

    def shift_imm(self, w, scalar=False):
        Q, U, immh, immb, opc, n, d = (bits(w, 30, 1), bits(w, 29, 1), bits(w, 19, 4), bits(w, 16, 3), bits(w, 11, 5),
                                       bits(w, 5, 5), bits(w, 0, 5))
        if not immh:
            raise Unsupported('shift immh')
        sz = immh.bit_length() - 1
        es = 8 << sz
        ih = (immh << 3) | immb
        rsh, lsh = 2 * es - ih, ih - es
        if scalar:
            Q = 1
        ne = 1 if scalar else lanes(Q, sz)
        narrow = 0b10000 <= opc <= 0b10011
        if opc in (0b11100, 0b11111):  # fixed-point conversions
            if sz < 2:
                raise Unsupported('fp16 fixed')
            if sz == 3 and not Q:
                raise Unsupported('fixed 1d')
            F, VT, VU, T, SF, SU, SET = FT[sz - 2]
            fb = rsh
            if opc == 0b11100:
                if scalar:
                    return f'{SET}({d}, fromint_{F}(c, {SU}({n}), {es}, {U}, {fb}));'
                return VWr(VT, Q, d, f'vfromint_{F}(c, {VR(VU, n)}, {fb}, {U})')
            if scalar:
                return f'c->v[{d}] = ({ut(sz)})toint_{F}(c, {SF}({n}), 3, {fb}, {es}, {U});'
            return VWr(VU, Q, d, f'vtoint_{F}(c, {VQ(VT, Q, n)}, 3, {fb}, {U})')
        if narrow:
            if sz == 3:
                raise Unsupported('narrow shift size')
            ne = 1 if scalar else 8 >> sz
            op = {(0b10000, 0): ('SHRN', 0), (0b10001, 0): ('RSHRN', 0), (0b10000, 1): ('QSHRUN', 0),
                  (0b10001, 1): ('QRSHRUN', 0), (0b10010, 0): ('QSHRN', 0), (0b10010, 1): ('QSHRN', 1),
                  (0b10011, 0): ('QRSHRN', 0), (0b10011, 1): ('QRSHRN', 1)}[(opc, U)]
            if scalar and op[0] in ('SHRN', 'RSHRN'):
                raise Unsupported('scalar shrn')
            part = 0 if scalar else Q
            return gen(lop(*op), sz, ne, d, sz, part * ne, part, n, sz + 1, 0, -1, 0, 0, -1, rsh)
        if opc == 0b10100:  # SSHLL / USHLL
            if scalar or sz == 3:
                raise Unsupported('sshll')
            ne = 8 >> sz
            return gen(lop('SHLLI', U), sz, ne, d, sz + 1, 0, 0, n, sz, Q * ne, -1, 0, 0, -1, lsh)
        if sz == 3 and not Q:
            raise Unsupported('shift 1d')
        if scalar and sz != 3 and opc not in (0b01100, 0b01110):
            raise Unsupported('scalar shift size')
        T, S = vu(sz), vs(sz)
        if opc in (0, 2):  # SSHR/USHR, SSRA/USRA
            if U:
                e = f'({T}){{}}' if rsh >= es else f'({VR(T, n)} >> {rsh})'
            else:
                e = f'({T})({VR(S, n)} >> {min(rsh, es - 1)})'
            if opc == 2:
                e = f'{VR(T, d)} + {e}'
            return VWr(T, Q, d, e) if not scalar else f'c->v[{d}] = (uint64_t)(({e})[0]);'
        if opc in (4, 6):
            return same(lop('RSHRI' if opc == 4 else 'RSRAI', U), sz, ne, d, n, -1, rsh)
        if opc == 8:
            if not U:
                raise Unsupported('sri U')
            return same(lop('SRI'), sz, ne, d, n, -1, rsh)
        if opc == 10:
            if U:
                return same(lop('SLI'), sz, ne, d, n, -1, lsh)
            e = f'{VR(T, n)} << {lsh}'
            return VWr(T, Q, d, e) if not scalar else f'c->v[{d}] = (uint64_t)(({e})[0]);'
        if opc == 12:
            if not U:
                raise Unsupported('sqshlu U')
            return same(lop('QSHLUI'), sz, ne, d, n, -1, lsh)
        if opc == 14:
            return same(lop('QSHLI', U), sz, ne, d, n, -1, lsh)
        raise Unsupported('shift imm opcode')

    # ------------------------------------------------------------ indexed element

    def indexed(self, w, scalar=False):
        Q, U, sz, L, M, rm, opc, H, n, d = (bits(w, 30, 1), bits(w, 29, 1), bits(w, 22, 2), bits(w, 21, 1),
                                           bits(w, 20, 1), bits(w, 16, 4), bits(w, 12, 4), bits(w, 11, 1),
                                           bits(w, 5, 5), bits(w, 0, 5))
        fp = opc in (1, 5, 9) and not (U and opc != 9)
        if fp:
            if sz < 2:
                raise Unsupported('fp16 indexed')
            fsz = sz & 1
            if fsz:
                if L:
                    raise Unsupported('indexed L')
                idx = H
            else:
                idx = (H << 1) | L
            m = (M << 4) | rm
            if not scalar and fsz and not Q:
                raise Unsupported('indexed 2d Q')
            F, VT, VU, T, SF, SU, SET = FT[fsz]
            if scalar:
                el = f'{VR(VT, m)}[{idx}]'
                if opc == 9:
                    return f'{SET}({d}, op_{F}(c, {"FOP_MULX" if U else "FOP_MUL"}, {SF}({n}), {el}, 0));'
                x = f'{SF}({n})' if opc == 1 else f'neg_{F}({SF}({n}))'
                return f'{SET}({d}, op_{F}(c, FOP_MADD, {SF}({d}), {x}, {el}));'
            el = f'vsplat_{F}({VR(VT, m)}, {idx}, {Q})'
            if opc == 9:
                return f'SV_FOP_{F.upper()}({"FOP_MULX" if U else "FOP_MUL"}, {Q}, {d}, {VQ(VT, Q, n)}, {el}, ({VT}){{}});'
            x = VQ(VT, Q, n) if opc == 1 else f'vneg_{F}({VQ(VT, Q, n)})'
            return f'SV_FOP_{F.upper()}(FOP_MADD, {Q}, {d}, {VQ(VT, Q, d)}, {x}, {el});'
        if sz == 1:
            idx, m = (H << 2) | (L << 1) | M, rm
        elif sz == 2:
            idx, m = (H << 1) | L, (M << 4) | rm
        else:
            raise Unsupported('indexed size')
        T = vu(sz)
        if scalar:
            op = {(0, 3): 'SQDMLAL', (0, 7): 'SQDMLSL', (0, 11): 'SQDMULL', (0, 12): 'SQDMULH',
                  (0, 13): 'SQRDMULH'}.get((U, opc))
            if op is None:
                raise Unsupported('scalar indexed')
            if op in ('SQDMULH', 'SQRDMULH'):
                return gen(lop(op), sz, 1, d, sz, 0, 0, n, sz, 0, m, sz, 0, idx)
            return gen(lop(op), sz, 1, d, sz + 1, 0, 0, n, sz, 0, m, sz, 0, idx)
        ne = lanes(Q, sz)
        el = shuf(T, VR(T, m), VR(T, m), [idx] * (16 >> sz))
        if (U, opc) == (0, 8):
            return VWr(T, Q, d, f'{VR(T, n)} * {el}')
        if (U, opc) in ((1, 0), (1, 4)):
            return VWr(T, Q, d, f'{VR(T, d)} {"+" if opc == 0 else "-"} {VR(T, n)} * {el}')
        if (U, opc) in ((0, 12), (0, 13)):
            return same(lop('SQDMULH' if opc == 12 else 'SQRDMULH'), sz, ne, d, n, m, 0, idx)
        lng = {2: 'MLAL', 6: 'MLSL', 10: 'MULL', 3: 'SQDMLAL', 7: 'SQDMLSL', 11: 'SQDMULL'}.get(opc)
        if lng is None or (U and lng.startswith('SQ')):
            raise Unsupported('indexed opcode')
        ne = 8 >> sz
        return gen(lop(lng, U if not lng.startswith('SQ') else 0), sz, ne, d, sz + 1, 0, 0, n, sz, Q * ne, m, sz, 0, idx)

    # ------------------------------------------------------------ scalar FP

    @staticmethod
    def _ftype(w):
        t = bits(w, 22, 2)
        if t > 1:
            raise Unsupported('fp type')
        return t

    def fp_dp3(self, w):
        t = self._ftype(w)
        o1, o0 = bits(w, 21, 1), bits(w, 15, 1)
        d, n, m, a = bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5), bits(w, 10, 5)
        F = FT[t][0]
        na, nn = o1, o1 ^ o0  # FMADD 00, FMSUB 01 (-n), FNMADD 10 (-a, -n), FNMSUB 11 (-a)
        return f'fp_fma_{F}(c, {na}, {nn}, {d}, {n}, {m}, {a});'

    def fp_dp2(self, w):
        t = self._ftype(w)
        opc, d, n, m = bits(w, 12, 4), bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5)
        fop = ('FOP_MUL', 'FOP_DIV', 'FOP_ADD', 'FOP_SUB', 'FOP_MAX', 'FOP_MIN', 'FOP_MAXNM', 'FOP_MINNM',
               'FOP_NMUL')[opc] if opc <= 8 else None
        if fop is None:
            raise Unsupported('fp dp2')
        return f'fp_op_{FT[t][0]}(c, {fop}, {d}, {n}, {m});'

    def fp_dp1(self, w):
        t = bits(w, 22, 2)
        opc, d, n = bits(w, 15, 6), bits(w, 0, 5), bits(w, 5, 5)
        if t == 3:  # half source: only FCVT
            if opc == 4:
                return f'FP_SETS({d}, simd_h2s(c, VS16({n})));'
            if opc == 5:
                return f'FP_SETD({d}, simd_h2d(c, VS16({n})));'
            raise Unsupported('fp16 dp1')
        if t == 2:
            raise Unsupported('fp type')
        F, VT, VU, T, SF, SU, SET = FT[t]
        sign = '0x80000000u' if not t else '0x8000000000000000ull'
        if opc == 0:
            return f'c->v[{d}] = {SU}({n});'
        if opc == 1:
            return f'c->v[{d}] = {SU}({n}) & ~{sign};'
        if opc == 2:
            return f'c->v[{d}] = {SU}({n}) ^ {sign};'
        if opc == 3:
            return f'fp_op_{F}(c, FOP_SQRT, {d}, {n}, {n});'
        if opc in (4, 5, 7):
            to = {4: 0, 5: 1, 7: 3}[opc]
            if to == t:
                raise Unsupported('fcvt same')
            if to == 3:
                return f'c->v[{d}] = {"simd_s2h" if not t else "simd_dd2h"}(c, {SF}({n}));'
            return f'FP_SETD({d}, simd_s2d(c, VSF32({n})));' if to else f'FP_SETS({d}, simd_d2s(c, VSF64({n})));'
        rm = {8: 0, 9: 1, 10: 2, 11: 3, 12: 4, 14: 5, 15: 6}.get(opc)
        if rm is None:
            raise Unsupported('fp dp1 opcode')
        return f'{SET}({d}, frint_{F}(c, {SF}({n}), {rm}));'

    def fp_cmp(self, w):
        t = self._ftype(w)
        if bits(w, 14, 2) or bits(w, 0, 3):
            raise Unsupported('fcmp op')
        F, VT, VU, T, SF, SU, SET = FT[t]
        n, m, zero = bits(w, 5, 5), bits(w, 16, 5), bits(w, 3, 1)
        b = '0' if zero else f'{SF}({m})'
        return f'c->nzcv = cmp_{F}(c, {SF}({n}), {b});'

    def fp_ccmp(self, w):
        t = self._ftype(w)
        F, VT, VU, T, SF, SU, SET = FT[t]
        n, m, cnd, nzcv = bits(w, 5, 5), bits(w, 16, 5), bits(w, 12, 4), bits(w, 0, 4)
        return f'c->nzcv = {cond(cnd)} ? cmp_{F}(c, {SF}({n}), {SF}({m})) : {hx(nzcv << 28)};'

    def fp_csel(self, w):
        t = self._ftype(w)
        SU = FT[t][5]
        d, n, m, cnd = bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5), bits(w, 12, 4)
        return f'c->v[{d}] = {cond(cnd)} ? {SU}({n}) : {SU}({m});'

    def fp_imm(self, w):
        t = self._ftype(w)
        if bits(w, 5, 5):
            raise Unsupported('fmov imm5')
        imm8, d = bits(w, 13, 8), bits(w, 0, 5)
        return f'c->v[{d}] = {hx(vfp_expand_imm(imm8, 32 << t))};'

    def fp_fixed(self, w):
        sf, t, rmode, opc, scale, n, d = (bits(w, 31, 1), bits(w, 22, 2), bits(w, 19, 2), bits(w, 16, 3), bits(w, 10, 6),
                                          bits(w, 5, 5), bits(w, 0, 5))
        if t > 1:
            raise Unsupported('fixed type')
        if not sf and scale < 32:
            raise Unsupported('fixed scale')
        F, VT, VU, T, SF, SU, SET = FT[t]
        fb, ib = 64 - scale, 64 if sf else 32
        if rmode == 0 and opc in (2, 3):
            src = X(n) if sf else W(n)
            return f'{SET}({d}, fromint_{F}(c, {src}, {ib}, {opc & 1}, {fb}));'
        if rmode == 3 and opc in (0, 1):
            return set_reg(sf, d, f'toint_{F}(c, {SF}({n}), 3, {fb}, {ib}, {opc})')
        raise Unsupported('fixed opcode')

    def fp_int(self, w):
        sf, t, rmode, opc, n, d = bits(w, 31, 1), bits(w, 22, 2), bits(w, 19, 2), bits(w, 16, 3), bits(w, 5, 5), bits(w, 0, 5)
        if opc in (6, 7):  # FMOV
            if t == 2 and sf and rmode == 1:
                return set_reg(1, d, f'VHI64({n})') if opc == 6 else f'VHI64({d}) = {X(n)};'
            if rmode or t != sf or t > 1:
                raise Unsupported('fmov general')
            if opc == 6:
                return set_reg(sf, d, f'VS64({n})' if sf else f'VS32({n})')
            return f'c->v[{d}] = {X(n) if sf else W(n)};'
        if t > 1:
            raise Unsupported('fp int type')
        F, VT, VU, T, SF, SU, SET = FT[t]
        ib = 64 if sf else 32
        if opc in (2, 3):
            if rmode:
                raise Unsupported('cvtf rmode')
            return f'{SET}({d}, fromint_{F}(c, {X(n) if sf else W(n)}, {ib}, {opc & 1}, 0));'
        if opc in (4, 5):
            if rmode:
                raise Unsupported('fcvta rmode')
            rm = 4
        else:
            rm = (0, 1, 2, 3)[rmode]
        return set_reg(sf, d, f'toint_{F}(c, {SF}({n}), {rm}, 0, {ib}, {opc & 1})')

    # ------------------------------------------------------------ loads / stores

    def ldst(self, w, pc):
        rt = bits(w, 0, 5)
        if bits(w, 27, 3) == 0b011 and bits(w, 24, 2) == 0:  # literal
            opc = bits(w, 30, 2)
            if opc == 3:
                raise Unsupported('ldr literal opc')
            addr = (pc + sext(bits(w, 5, 19), 19) * 4) & ((1 << 64) - 1)
            return f'c->v[{rt}] = {("LD32", "LD64", "LD128")[opc]}({hx(addr)});'
        if bits(w, 31, 1) == 0 and bits(w, 24, 6) in (0b001100, 0b001101):
            return self.ldst_struct(w)
        if bits(w, 27, 3) == 0b101:
            return self.ldst_pair(w)
        if bits(w, 27, 3) == 0b111:
            return self.ldst_reg(w)
        raise Unsupported(f'ldst simd {w:08x}')

    def ldst_pair(self, w):
        opc, mode, L = bits(w, 30, 2), bits(w, 23, 2), bits(w, 22, 1)
        imm7, rt2, rn, rt = sext(bits(w, 15, 7), 7), bits(w, 10, 5), bits(w, 5, 5), bits(w, 0, 5)
        if opc == 3:
            raise Unsupported('pair opc 3')
        scale = 2 + opc
        off = imm7 << scale
        base = XS(rn)
        post, wb = mode == 1, mode in (1, 3)
        ea = base if post else (f'{base} + {off}' if off >= 0 else f'{base} - {-off}')
        ld, st = {2: ('LD32', 'ST32'), 3: ('LD64', 'ST64'), 4: ('LD128', 'ST128')}[scale]
        sz = 1 << scale
        lines = [f'uint64_t ea = {ea};']
        if L:
            lines.append(f'u128 a = {ld}(ea), b = {ld}(ea + {sz});')
            lines.append(f'c->v[{rt}] = a; c->v[{rt2}] = b;')
        else:
            cast = {2: 'VS32', 3: 'VS64', 4: 'c->v'}[scale]
            v1, v2 = (f'c->v[{rt}]', f'c->v[{rt2}]') if scale == 4 else (f'{cast}({rt})', f'{cast}({rt2})')
            lines.append(f'{st}(ea, {v1}); {st}(ea + {sz}, {v2});')
        if wb:
            dst = 'c->sp' if rn == 31 else f'c->x[{rn}]'
            if not post:
                lines.append(f'{dst} = ea;')
            else:
                lines.append(f'{dst} = ea + {off};' if off >= 0 else f'{dst} = ea - {-off};')
        return '{ ' + ' '.join(lines) + ' }'

    def ldst_reg(self, w):
        size, opc, rn, rt = bits(w, 30, 2), bits(w, 22, 2), bits(w, 5, 5), bits(w, 0, 5)
        if opc & 2:
            if size:
                raise Unsupported('ldst simd size')
            scale = 4
        else:
            scale = size
        L = opc & 1
        base = XS(rn)
        wb = None
        if bits(w, 24, 2) == 1:
            ea = f'{base} + {hx(bits(w, 10, 12) << scale)}'
        elif bits(w, 24, 2) == 0:
            kind = bits(w, 10, 2)
            if bits(w, 21, 1):
                if kind != 2:
                    raise Unsupported('ldst simd reg kind')
                rm, option, S = bits(w, 16, 5), bits(w, 13, 3), bits(w, 12, 1)
                if option not in (2, 3, 6, 7):
                    raise Unsupported('ldst simd reg option')
                ea = f'{base} + {extend_expr(rm, option, scale if S else 0)}'
            else:
                imm9 = sext(bits(w, 12, 9), 9)
                si = f'+ {imm9}' if imm9 >= 0 else f'- {-imm9}'
                if kind == 0:
                    ea = f'{base} {si}'
                elif kind == 1:
                    ea, wb = base, f'{base} {si}'
                elif kind == 3:
                    ea, wb = f'{base} {si}', 'ea'
                else:
                    raise Unsupported('ldst simd unprivileged')
        else:
            raise Unsupported('ldst simd')
        parts = [f'uint64_t ea = {ea};']
        if L:
            parts.append(f'c->v[{rt}] = {("LD8", "LD16", "LD32", "LD64", "LD128")[scale]}(ea);')
        else:
            src = (f'VS8({rt})', f'VS16({rt})', f'VS32({rt})', f'VS64({rt})', f'c->v[{rt}]')[scale]
            parts.append(f'{("ST8", "ST16", "ST32", "ST64", "ST128")[scale]}(ea, {src});')
        if wb:
            parts.append(f'{"c->sp" if rn == 31 else f"c->x[{rn}]"} = {wb};')
        return '{ ' + ' '.join(parts) + ' }'

    def ldst_struct(self, w):
        Q, L, post, rm, rn, rt, sz = (bits(w, 30, 1), bits(w, 22, 1), bits(w, 23, 1), bits(w, 16, 5), bits(w, 5, 5),
                                      bits(w, 0, 5), bits(w, 10, 2))
        if not post and rm:
            raise Unsupported('ldst struct rm')
        base = XS(rn)
        if bits(w, 24, 1) == 0:  # multiple structures
            if bits(w, 21, 1):
                raise Unsupported('ldst multiple bit21')
            opc = bits(w, 12, 4)
            sel = {0: (4, 1), 2: (1, 4), 4: (3, 1), 6: (1, 3), 7: (1, 1), 8: (2, 1), 10: (1, 2)}.get(opc)
            if sel is None:
                raise Unsupported('ldst multiple opcode')
            selem, rpt = sel
            if sz == 3 and not Q and selem > 1:
                raise Unsupported('ldst multiple 1d')
            total = (8 << Q) * selem * rpt
            if selem == 1:  # LD1/ST1: whole registers
                regs = [(rt + i) % 32 for i in range(rpt)]
                step = 16 if Q else 8
                if L:
                    acc = ' '.join(f'c->v[{r}] = {"LD128" if Q else "LD64"}(ea + {i * step});' for i, r in enumerate(regs))
                else:
                    acc = ' '.join(f'{"ST128" if Q else "ST64"}(ea + {i * step}, {"c->v[%d]" % r if Q else "VS64(%d)" % r});'
                                   for i, r in enumerate(regs))
            else:
                acc = f'sl_{"ldm" if L else "stm"}(c, {Q}, {sz}, {selem}, {rpt}, {rt}, ea);'
        else:  # single structure
            opc, S, R = bits(w, 13, 3), bits(w, 12, 1), bits(w, 21, 1)
            scale = opc >> 1
            selem = ((opc & 1) << 1 | R) + 1
            if scale == 3:
                if not L or S:
                    raise Unsupported('ldnr')
                total = selem << sz
                acc = f'sl_ldr(c, {Q}, {sz}, {selem}, {rt}, ea);'
            else:
                if scale == 0:
                    idx = (Q << 3) | (S << 2) | sz
                elif scale == 1:
                    if sz & 1:
                        raise Unsupported('ld1 h size')
                    idx = (Q << 2) | (S << 1) | (sz >> 1)
                else:
                    if sz & 2:
                        raise Unsupported('ld1 s size')
                    if sz & 1:
                        if S:
                            raise Unsupported('ld1 d S')
                        idx, scale = Q, 3
                    else:
                        idx = (Q << 1) | S
                total = selem << scale
                acc = f'sl_{"lds" if L else "sts"}(c, {scale}, {selem}, {idx}, {rt}, ea);'
        parts = [f'uint64_t ea = {base};', acc]
        if post:
            dst = 'c->sp' if rn == 31 else f'c->x[{rn}]'
            parts.append(f'{dst} = ea + {total};' if rm == 31 else f'{dst} = ea + {X(rm)};')
        return '{ ' + ' '.join(parts) + ' }'

    # ------------------------------------------------------------ CRC32

    @staticmethod
    def crc(w):
        sf, rm, C, sz, rn, rd = bits(w, 31, 1), bits(w, 16, 5), bits(w, 12, 1), bits(w, 10, 2), bits(w, 5, 5), bits(w, 0, 5)
        if (sz == 3) != bool(sf):
            raise Unsupported('crc size')
        val = X(rm) if sz == 3 else W(rm)
        return set_reg(0, rd, f'{"CRC32C" if C else "CRC32"}({W(rn)}, {val}, {1 << sz})')

