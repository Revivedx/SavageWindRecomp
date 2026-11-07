// Portable C lowering of AArch64 FP / AdvSIMD / crypto / CRC32 (tools/simd_c.py emits calls with constant
// fields; everything is always_inline so the size/lane/op switches fold at -O1).
// Semantics follow the ARM pseudocode, not the host: NaN propagation (first signalling NaN quietened, then
// first quiet NaN), FPCR.DN default NaN, FPCR.FZ flushing (inputs and pre-rounding-tiny results), FPCR.RMode
// for arithmetic / conversions / FRINTX/I, saturating FP->int conversions, fused multiply-add, ARM estimate
// tables. Not modelled: FPSR cumulative exception bits and QC, FPCR trap enables, FP16 arithmetic (ARMv8.2).
// FPCR.AHP is honoured by the half-precision conversions. Host assumptions: IEEE binary32/64, MXCSR (or the
// host FPU) in round-to-nearest with denormals enabled; fma must be correctly rounded (x86-64: the FMA instructions
// with -mfma, else simd_softfma.h).
#pragma once
#include <fenv.h>
#include <math.h>

#include "cpu.h"

typedef __uint128_t u128;
#define SI static inline __attribute__((always_inline))
#define SN static __attribute__((noinline))
#define SIMD_CAT2(a, b) a##_##b
#define SIMD_CAT(a, b) SIMD_CAT2(a, b)

#define SIMD_VT(name, T) typedef T name __attribute__((vector_size(16), may_alias));
SIMD_VT(vu8, uint8_t) SIMD_VT(vs8, int8_t) SIMD_VT(vu16, uint16_t) SIMD_VT(vs16, int16_t)
SIMD_VT(vu32, uint32_t) SIMD_VT(vs32, int32_t) SIMD_VT(vu64, uint64_t) SIMD_VT(vs64, int64_t)
SIMD_VT(vf32, float) SIMD_VT(vf64, double)
typedef uint16_t __attribute__((may_alias)) sa16;
typedef uint32_t __attribute__((may_alias)) sa32;
typedef uint64_t __attribute__((may_alias)) sa64;
typedef float __attribute__((may_alias)) saf32;
typedef double __attribute__((may_alias)) saf64;
typedef u128 __attribute__((aligned(1), may_alias)) u128u;

// ---- registers: VREG = whole V register as a vector type; VQ = source with the upper half cleared when
// Q=0; VW = write, zeroing the upper half when Q=0. Scalar views of lane 0: VS8..VS64, VSF32/VSF64.
#define VREG(T, n) (*(T *)&c->v[n])
#define VQ(T, Q, n) ((T)((Q) ? c->v[n] : (u128)(uint64_t)c->v[n]))
#define VW(T, Q, d, e) do { T _w = (e); c->v[d] = (Q) ? (u128)_w : (u128)(uint64_t)(u128)_w; } while (0)
#define VS8(n) (*(uint8_t *)&c->v[n])
#define VS16(n) (*(sa16 *)&c->v[n])
#define VS32(n) (*(sa32 *)&c->v[n])
#define VS64(n) (*(sa64 *)&c->v[n])
#define VSF32(n) (*(saf32 *)&c->v[n])
#define VSF64(n) (*(saf64 *)&c->v[n])
#define VHI64(n) (((sa64 *)&c->v[n])[1])
#define LD128(a) (*(const u128u *)(uintptr_t)(a))
#define ST128(a, v) (*(u128u *)(uintptr_t)(a) = (u128)(v))
#define SHUF __builtin_shufflevector
#define CVT __builtin_convertvector

#define FPCR_AHP (1u << 26)
#define FPCR_DN (1u << 25)
#define FPCR_FZ (1u << 24)
#define FPCR_RM (3u << 22)
// FPCR settings the fast paths below can't follow: they go through the slow ones. On x86-64 FZ isn't one of them: the
// thread's MXCSR follows it (rt_fpcr_write, below).
#if defined(__x86_64__)
#define FPCR_SLOW FPCR_RM
#else
#define FPCR_SLOW (FPCR_FZ | FPCR_RM)
#endif

enum { FOP_ADD, FOP_SUB, FOP_MUL, FOP_MULX, FOP_NMUL, FOP_DIV, FOP_ABD, FOP_SQRT, FOP_MADD, FOP_RECPS, FOP_RSQRTS,
       FOP_MAX, FOP_MIN, FOP_MAXNM, FOP_MINNM };

static const uint8_t simd_recip_tab[256] = {
    255, 253, 251, 249, 247, 245, 243, 241, 240, 238, 236, 234, 232, 230, 229, 227,
    225, 223, 221, 220, 218, 216, 215, 213, 211, 210, 208, 206, 205, 203, 201, 200,
    198, 197, 195, 194, 192, 191, 189, 188, 186, 185, 183, 182, 180, 179, 177, 176,
    174, 173, 172, 170, 169, 167, 166, 165, 163, 162, 161, 159, 158, 157, 156, 154,
    153, 152, 150, 149, 148, 147, 145, 144, 143, 142, 141, 139, 138, 137, 136, 135,
    134, 132, 131, 130, 129, 128, 127, 126, 124, 123, 122, 121, 120, 119, 118, 117,
    116, 115, 114, 113, 112, 111, 110, 109, 108, 107, 106, 105, 104, 103, 102, 101,
    100, 99, 98, 97, 96, 95, 94, 93, 92, 91, 90, 89, 88, 88, 87, 86,
    85, 84, 83, 82, 81, 81, 80, 79, 78, 77, 76, 75, 75, 74, 73, 72,
    71, 70, 70, 69, 68, 67, 66, 66, 65, 64, 63, 63, 62, 61, 60, 59,
    59, 58, 57, 56, 56, 55, 54, 53, 53, 52, 51, 51, 50, 49, 48, 48,
    47, 46, 46, 45, 44, 44, 43, 42, 42, 41, 40, 40, 39, 38, 38, 37,
    36, 36, 35, 34, 34, 33, 32, 32, 31, 30, 30, 29, 29, 28, 27, 27,
    26, 26, 25, 24, 24, 23, 23, 22, 21, 21, 20, 20, 19, 18, 18, 17,
    17, 16, 16, 15, 15, 14, 13, 13, 12, 12, 11, 11, 10, 10, 9, 9,
    8, 7, 7, 6, 6, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0,
};
static const uint8_t simd_rsqrt_tab[384] = {
    255, 253, 251, 249, 247, 245, 243, 242, 240, 238, 236, 234, 233, 231, 229, 228,
    226, 224, 223, 221, 219, 218, 216, 215, 213, 212, 210, 209, 207, 206, 204, 203,
    201, 200, 198, 197, 196, 194, 193, 192, 190, 189, 188, 186, 185, 184, 183, 181,
    180, 179, 178, 176, 175, 174, 173, 172, 170, 169, 168, 167, 166, 165, 164, 163,
    162, 160, 159, 158, 157, 156, 155, 154, 153, 152, 151, 150, 149, 148, 147, 146,
    145, 144, 143, 142, 141, 140, 140, 139, 138, 137, 136, 135, 134, 133, 132, 131,
    131, 130, 129, 128, 127, 126, 126, 125, 124, 123, 122, 121, 121, 120, 119, 118,
    118, 117, 116, 115, 114, 114, 113, 112, 111, 111, 110, 109, 109, 108, 107, 106,
    105, 105, 104, 104, 103, 103, 101, 101, 100, 100, 99, 99, 97, 97, 96, 96,
    95, 95, 93, 93, 92, 92, 91, 91, 90, 90, 88, 88, 87, 87, 86, 86,
    85, 85, 84, 84, 82, 82, 81, 81, 80, 80, 79, 79, 78, 78, 77, 77,
    76, 76, 75, 75, 74, 74, 72, 72, 71, 71, 70, 70, 69, 69, 68, 68,
    67, 67, 66, 66, 65, 65, 64, 64, 63, 63, 62, 62, 61, 61, 60, 60,
    60, 60, 59, 59, 58, 58, 57, 57, 56, 56, 55, 55, 54, 54, 53, 53,
    52, 52, 51, 51, 51, 51, 50, 50, 49, 49, 48, 48, 47, 47, 46, 46,
    46, 46, 45, 45, 44, 44, 43, 43, 42, 42, 42, 42, 41, 41, 40, 40,
    39, 39, 38, 38, 38, 38, 37, 37, 36, 36, 35, 35, 35, 35, 34, 34,
    33, 33, 33, 33, 32, 32, 31, 31, 30, 30, 30, 30, 29, 29, 28, 28,
    28, 28, 27, 27, 26, 26, 26, 26, 25, 25, 24, 24, 24, 24, 23, 23,
    22, 22, 22, 22, 21, 21, 20, 20, 20, 20, 19, 19, 19, 19, 18, 18,
    17, 17, 17, 17, 16, 16, 16, 16, 15, 15, 14, 14, 14, 14, 13, 13,
    13, 13, 12, 12, 11, 11, 11, 11, 10, 10, 10, 10, 9, 9, 9, 9,
    8, 8, 8, 8, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4,
    4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0,
};
static const uint32_t simd_crc32_tab[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};
static const uint32_t simd_crc32c_tab[256] = {
    0x00000000, 0xf26b8303, 0xe13b70f7, 0x1350f3f4, 0xc79a971f, 0x35f1141c, 0x26a1e7e8, 0xd4ca64eb,
    0x8ad958cf, 0x78b2dbcc, 0x6be22838, 0x9989ab3b, 0x4d43cfd0, 0xbf284cd3, 0xac78bf27, 0x5e133c24,
    0x105ec76f, 0xe235446c, 0xf165b798, 0x030e349b, 0xd7c45070, 0x25afd373, 0x36ff2087, 0xc494a384,
    0x9a879fa0, 0x68ec1ca3, 0x7bbcef57, 0x89d76c54, 0x5d1d08bf, 0xaf768bbc, 0xbc267848, 0x4e4dfb4b,
    0x20bd8ede, 0xd2d60ddd, 0xc186fe29, 0x33ed7d2a, 0xe72719c1, 0x154c9ac2, 0x061c6936, 0xf477ea35,
    0xaa64d611, 0x580f5512, 0x4b5fa6e6, 0xb93425e5, 0x6dfe410e, 0x9f95c20d, 0x8cc531f9, 0x7eaeb2fa,
    0x30e349b1, 0xc288cab2, 0xd1d83946, 0x23b3ba45, 0xf779deae, 0x05125dad, 0x1642ae59, 0xe4292d5a,
    0xba3a117e, 0x4851927d, 0x5b016189, 0xa96ae28a, 0x7da08661, 0x8fcb0562, 0x9c9bf696, 0x6ef07595,
    0x417b1dbc, 0xb3109ebf, 0xa0406d4b, 0x522bee48, 0x86e18aa3, 0x748a09a0, 0x67dafa54, 0x95b17957,
    0xcba24573, 0x39c9c670, 0x2a993584, 0xd8f2b687, 0x0c38d26c, 0xfe53516f, 0xed03a29b, 0x1f682198,
    0x5125dad3, 0xa34e59d0, 0xb01eaa24, 0x42752927, 0x96bf4dcc, 0x64d4cecf, 0x77843d3b, 0x85efbe38,
    0xdbfc821c, 0x2997011f, 0x3ac7f2eb, 0xc8ac71e8, 0x1c661503, 0xee0d9600, 0xfd5d65f4, 0x0f36e6f7,
    0x61c69362, 0x93ad1061, 0x80fde395, 0x72966096, 0xa65c047d, 0x5437877e, 0x4767748a, 0xb50cf789,
    0xeb1fcbad, 0x197448ae, 0x0a24bb5a, 0xf84f3859, 0x2c855cb2, 0xdeeedfb1, 0xcdbe2c45, 0x3fd5af46,
    0x7198540d, 0x83f3d70e, 0x90a324fa, 0x62c8a7f9, 0xb602c312, 0x44694011, 0x5739b3e5, 0xa55230e6,
    0xfb410cc2, 0x092a8fc1, 0x1a7a7c35, 0xe811ff36, 0x3cdb9bdd, 0xceb018de, 0xdde0eb2a, 0x2f8b6829,
    0x82f63b78, 0x709db87b, 0x63cd4b8f, 0x91a6c88c, 0x456cac67, 0xb7072f64, 0xa457dc90, 0x563c5f93,
    0x082f63b7, 0xfa44e0b4, 0xe9141340, 0x1b7f9043, 0xcfb5f4a8, 0x3dde77ab, 0x2e8e845f, 0xdce5075c,
    0x92a8fc17, 0x60c37f14, 0x73938ce0, 0x81f80fe3, 0x55326b08, 0xa759e80b, 0xb4091bff, 0x466298fc,
    0x1871a4d8, 0xea1a27db, 0xf94ad42f, 0x0b21572c, 0xdfeb33c7, 0x2d80b0c4, 0x3ed04330, 0xccbbc033,
    0xa24bb5a6, 0x502036a5, 0x4370c551, 0xb11b4652, 0x65d122b9, 0x97baa1ba, 0x84ea524e, 0x7681d14d,
    0x2892ed69, 0xdaf96e6a, 0xc9a99d9e, 0x3bc21e9d, 0xef087a76, 0x1d63f975, 0x0e330a81, 0xfc588982,
    0xb21572c9, 0x407ef1ca, 0x532e023e, 0xa145813d, 0x758fe5d6, 0x87e466d5, 0x94b49521, 0x66df1622,
    0x38cc2a06, 0xcaa7a905, 0xd9f75af1, 0x2b9cd9f2, 0xff56bd19, 0x0d3d3e1a, 0x1e6dcdee, 0xec064eed,
    0xc38d26c4, 0x31e6a5c7, 0x22b65633, 0xd0ddd530, 0x0417b1db, 0xf67c32d8, 0xe52cc12c, 0x1747422f,
    0x49547e0b, 0xbb3ffd08, 0xa86f0efc, 0x5a048dff, 0x8ecee914, 0x7ca56a17, 0x6ff599e3, 0x9d9e1ae0,
    0xd3d3e1ab, 0x21b862a8, 0x32e8915c, 0xc083125f, 0x144976b4, 0xe622f5b7, 0xf5720643, 0x07198540,
    0x590ab964, 0xab613a67, 0xb831c993, 0x4a5a4a90, 0x9e902e7b, 0x6cfbad78, 0x7fab5e8c, 0x8dc0dd8f,
    0xe330a81a, 0x115b2b19, 0x020bd8ed, 0xf0605bee, 0x24aa3f05, 0xd6c1bc06, 0xc5914ff2, 0x37faccf1,
    0x69e9f0d5, 0x9b8273d6, 0x88d28022, 0x7ab90321, 0xae7367ca, 0x5c18e4c9, 0x4f48173d, 0xbd23943e,
    0xf36e6f75, 0x0105ec76, 0x12551f82, 0xe03e9c81, 0x34f4f86a, 0xc69f7b69, 0xd5cf889d, 0x27a40b9e,
    0x79b737ba, 0x8bdcb4b9, 0x988c474d, 0x6ae7c44e, 0xbe2da0a5, 0x4c4623a6, 0x5f16d052, 0xad7d5351,
};
static const uint8_t simd_aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};
static const uint8_t simd_aes_isbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

SI int simd_fe_mode(uint64_t fpcr) {
    static const int m[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    return m[(fpcr >> 22) & 3];
}

// The host's rounding mode for the FPCR paths below. On x86-64 float and double math is SSE, so only MXCSR's
// rounding field matters: MinGW's fegetround / fesetround also read and rewrite the x87 control word, and cost more
// than the operation they wrapped. (BOTW 1.8.2 sets FPCR.FZ, which sends every scalar op down the slow path: its
// main thread spent half its time there.) The paths change the mode only when FPCR asks for one other than
// round to nearest, the host's own, and put it back.
#if defined(__x86_64__)
#include <xmmintrin.h>
// (MXCSR.RC: 0 nearest, 1 down, 2 up, 3 toward zero. The FE_* values differ between C libraries: UCRT's are 0x100
// apart, glibc's and msvcrt's 0x400.)
SI unsigned simd_rc(int fe) { return fe == FE_DOWNWARD ? 1u : fe == FE_UPWARD ? 2u : fe == FE_TOWARDZERO ? 3u : 0u; }
SI int simd_getround(void) {
    static const int m[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    return m[(_mm_getcsr() >> 13) & 3u];
}
SI void simd_setround(int fe) { _mm_setcsr((_mm_getcsr() & ~0x6000u) | simd_rc(fe) << 13); }
// MSR FPCR. FZ (flush denormal inputs and results to signed zero) is MXCSR's DAZ (inputs) and FTZ (results) on x86-64,
// so the FP ops keep their fast paths: BOTW 1.8.2 sets FZ at the start of its threads (main 0x7000b4de30, the thread
// entry 0x70000c4e3c), and the software flush sent every FP op down the slow path (~40% of the main thread's time in
// the open world). Each guest thread is a host thread of its own whose FP code is the guest's, and lifted FPCR writes
// are the only ones (new threads start with FPCR 0 and the host's default MXCSR). The one difference: a result that
// rounds up to the smallest normal may be kept, where ARM (tininess before rounding) flushes it.
SI void rt_fpcr_write(Cpu *c, uint64_t v) {
    c->fpcr = v & 0x7c00000ull;
    unsigned m = _mm_getcsr() & ~0x8040u;  // (FTZ bit 15, DAZ bit 6)
    _mm_setcsr(c->fpcr & FPCR_FZ ? m | 0x8040u : m);
}
#else
SI int simd_getround(void) { return fegetround(); }
SI void simd_setround(int fe) { fesetround(fe); }
SI void rt_fpcr_write(Cpu *c, uint64_t v) { c->fpcr = v & 0x7c00000ull; }
#endif

// Fused multiply-add (simd_fp.h SIMD_FMA): the FMA instructions where the target has them; x86-64 game code built
// without them (the compatible target, runtime/gamebuild.c) uses its own, which follows MXCSR as they do.
#if defined(__x86_64__) && !defined(__FMA__)
#include "simd_softfma.h"
#define SIMD_SOFT_FMA 1
// Set (by runtime/gamelib.c, from settings.h fast_fma): x*y + z with two roundings in place of the exact fma, which
// costs the compatible target ~10% of its frame rate. The last bit of a result can differ from the console's.
#ifndef _WIN32
__attribute__((visibility("hidden")))
#endif
extern int simd_fma_unfused;
#endif

#define F s
#define T float
#define U uint32_t
#define NB 32
#define EB 8
#define MB 23
#define VT vf32
#define VTU vu32
#define VTS vs32
#define NL 4
#include "simd_fp.h"
#undef F
#undef T
#undef U
#undef NB
#undef EB
#undef MB
#undef VT
#undef VTU
#undef VTS
#undef NL
#define F d
#define T double
#define U uint64_t
#define NB 64
#define EB 11
#define MB 52
#define VT vf64
#define VTU vu64
#define VTS vs64
#define NL 2
#include "simd_fp.h"
#undef F
#undef T
#undef U
#undef NB
#undef EB
#undef MB
#undef VT
#undef VTU
#undef VTS
#undef NL

// ---- precision conversions (FCVT, FCVTL/N, FCVTXN); FZ flushes single/double inputs, never halves
SI double simd_s2d(Cpu *c, float x) {
    uint32_t u = fb_s(fz_s(c, x));
    if (isnan_s(u)) {
        if (c->fpcr & FPCR_DN) return fv_d(0x7ff8000000000000ull);
        return fv_d(((uint64_t)(u >> 31) << 63) | 0x7ff8000000000000ull | ((uint64_t)(u & 0x3fffff) << 29));
    }
    return (double)fv_s(u);
}
SN float simd_d2s_slow(Cpu *c, double x) {
#pragma STDC FENV_ACCESS ON
    int mode = simd_fe_mode(c->fpcr);
    if (mode == FE_TONEAREST) return (float)x;  // (FZ alone: flushed by the caller)
    int old = simd_getround();
    simd_setround(mode);
    volatile double vx = x;
    volatile float r = (float)vx;
    simd_setround(old);
    return r;
}
SI float simd_d2s(Cpu *c, double x) {
    uint64_t u = fb_d(fz_d(c, x));
    if (isnan_d(u)) {
        if (c->fpcr & FPCR_DN) return fv_s(0x7fc00000u);
        return fv_s((uint32_t)(u >> 63) << 31 | 0x7fc00000u | (uint32_t)((u >> 29) & 0x3fffff));
    }
    x = fv_d(u);
    if (__builtin_expect(c->fpcr & FPCR_SLOW, 0)) {
        if ((c->fpcr & FPCR_FZ) && __builtin_fabs(x) < 0x1p-126 && x != 0) return fv_s((uint32_t)(u >> 63) << 31);
        return simd_d2s_slow(c, x);
    }
    return (float)x;
}
// round to odd (FCVTXN)
SI float simd_d2s_odd(Cpu *c, double x) {
    uint64_t u = fb_d(fz_d(c, x));
    if (isnan_d(u)) return simd_d2s(c, x);
    x = fv_d(u);
    if ((c->fpcr & FPCR_FZ) && __builtin_fabs(x) < 0x1p-126 && x != 0) return fv_s((uint32_t)(u >> 63) << 31);
    float f = (float)x;
    if ((double)f != x && x == x) {
        uint32_t b = fb_s(f);
        if (__builtin_fabs((double)f) > __builtin_fabs(x)) b--;
        return fv_s(b | 1);
    }
    return f;
}
SI uint64_t simd_rnd_shift(uint64_t m, int sh, int rm, int neg) {
    if (sh >= 64) return (rm == 1 && !neg && m) || (rm == 2 && neg && m);
    uint64_t q = m >> sh, rem = m & ((1ull << sh) - 1), half = 1ull << (sh - 1);
    if (rm == 0) q += rem > half || (rem == half && (q & 1));
    else if (rm == 1) q += rem && !neg;
    else if (rm == 2) q += rem && neg;
    return q;
}
// double (exact image of the single/double source, NaN payload left-aligned) -> half, FPCR rounding
SI uint16_t simd_d2h(Cpu *c, uint64_t u) {
    uint64_t fpcr = c->fpcr;
    int ahp = !!(fpcr & FPCR_AHP), rm = (fpcr >> 22) & 3, neg = (int)(u >> 63);
    uint16_t s = (uint16_t)(neg << 15);
    int e = (int)((u >> 52) & 0x7ff);
    uint64_t f = u & ((1ull << 52) - 1);
    if (e == 0x7ff) {
        if (f) return ahp ? s : (fpcr & FPCR_DN) ? 0x7e00 : (uint16_t)(s | 0x7e00 | (f >> 42));
        return ahp ? (uint16_t)(s | 0x7fff) : (uint16_t)(s | 0x7c00);
    }
    if (!e && !f) return s;
    uint64_t m = e ? f | (1ull << 52) : f;
    int E = e ? e - 1075 : -1074, ue = e ? e - 1023 : -1023;
    uint64_t q;
    int bits;
    if (ue < -14) {
        q = simd_rnd_shift(m, -24 - E, rm, neg);
        bits = (int)q;
    } else {
        q = simd_rnd_shift(m, 42, rm, neg);
        bits = ((ue + 14) << 10) + (int)q;
    }
    if (ahp) return (uint16_t)(s | (bits > 0x7fff ? 0x7fff : bits));
    if (bits >= 0x7c00) {
        int inf = rm == 0 || (rm == 1 && !neg) || (rm == 2 && neg);
        return (uint16_t)(s | (inf ? 0x7c00 : 0x7bff));
    }
    return (uint16_t)(s | bits);
}
SI uint16_t simd_s2h(Cpu *c, float x) {
    uint32_t u = fb_s(fz_s(c, x));
    uint64_t d = isnan_s(u) ? ((uint64_t)(u >> 31) << 63) | 0x7ff0000000000000ull | ((uint64_t)(u & 0x7fffff) << 29)
                            : fb_d((double)fv_s(u));
    return simd_d2h(c, d);
}
SI uint16_t simd_dd2h(Cpu *c, double x) { return simd_d2h(c, fb_d(fz_d(c, x))); }
SI uint64_t simd_h2dbits(Cpu *c, uint16_t h) {
    uint64_t s = (uint64_t)(h >> 15) << 63;
    int e = (h >> 10) & 31, f = h & 0x3ff;
    if (e == 31 && !(c->fpcr & FPCR_AHP)) {
        if (!f) return s | 0x7ff0000000000000ull;
        if (c->fpcr & FPCR_DN) return 0x7ff8000000000000ull;
        return s | 0x7ff8000000000000ull | ((uint64_t)f << 42);
    }
    if (!e) return fb_d((s ? -1.0 : 1.0) * f * 0x1p-24);
    return s | ((uint64_t)(e - 15 + 1023) << 52) | ((uint64_t)f << 42);
}
SI double simd_h2d(Cpu *c, uint16_t h) { return fv_d(simd_h2dbits(c, h)); }
SI float simd_h2s(Cpu *c, uint16_t h) {
    uint64_t d = simd_h2dbits(c, h);
    if (isnan_d(d)) return fv_s((uint32_t)(d >> 63) << 31 | 0x7fc00000u | (uint32_t)((d >> 29) & 0x3fffff));
    return (float)fv_d(d);
}

// ---- scalar FP instructions (FP data processing)
#define FP_SETS(d, x) (c->v[d] = fb_s(x))
#define FP_SETD(d, x) (c->v[d] = fb_d(x))
SI void fp_op_s(Cpu *c, int op, int d, int n, int m) { FP_SETS(d, op_s(c, op, VSF32(n), VSF32(m), 0)); }
SI void fp_op_d(Cpu *c, int op, int d, int n, int m) { FP_SETD(d, op_d(c, op, VSF64(n), VSF64(m), 0)); }
// FMADD family: neg_a / neg_n negate the addend / first factor before the fused op
SI void fp_fma_s(Cpu *c, int na, int nn, int d, int n, int m, int a) {
    float x = VSF32(a), y = VSF32(n);
    if (na) x = neg_s(x);
    if (nn) y = neg_s(y);
    FP_SETS(d, op_s(c, FOP_MADD, x, y, VSF32(m)));
}
SI void fp_fma_d(Cpu *c, int na, int nn, int d, int n, int m, int a) {
    double x = VSF64(a), y = VSF64(n);
    if (na) x = neg_d(x);
    if (nn) y = neg_d(y);
    FP_SETD(d, op_d(c, FOP_MADD, x, y, VSF64(m)));
}

// ---- integer lanes (complex ops run lane by lane; constant sizes fold to plain loads/stores)
SI uint64_t lmask(int sz) { return sz == 3 ? ~0ull : (1ull << (8 << sz)) - 1; }
SI int64_t lsx(uint64_t v, int sz) { int s = 64 - (8 << sz); return (int64_t)(v << s) >> s; }
SI uint64_t lget(const u128 *v, int sz, int i) {
    const uint8_t *p = (const uint8_t *)v + (i << sz);
    switch (sz) {
    case 0: return *p;
    case 1: return *(const sa16 *)p;
    case 2: return *(const sa32 *)p;
    default: return *(const sa64 *)p;
    }
}
SI void lset(u128 *v, int sz, int i, uint64_t x) {
    uint8_t *p = (uint8_t *)v + (i << sz);
    switch (sz) {
    case 0: *p = (uint8_t)x; break;
    case 1: *(sa16 *)p = (uint16_t)x; break;
    case 2: *(sa32 *)p = (uint32_t)x; break;
    default: *(sa64 *)p = x; break;
    }
}
SI uint64_t sat_s(__int128 v, int sz) {
    __int128 hi = ((__int128)1 << ((8 << sz) - 1)) - 1, lo = -hi - 1;
    return (uint64_t)(v > hi ? hi : v < lo ? lo : v) & lmask(sz);
}
SI uint64_t sat_u(__int128 v, int sz) {
    __int128 hi = ((__int128)1 << (8 << sz)) - 1;
    return (uint64_t)(v > hi ? hi : v < 0 ? 0 : v);
}
SI uint64_t sat_uu(unsigned __int128 v, int sz) { return v > lmask(sz) ? lmask(sz) : (uint64_t)v; }
SI __int128 shl128(__int128 v, int s) { return (__int128)((unsigned __int128)v << s); }
// right shift by s >= 1 with optional rounding, as an infinite-precision value
SI __int128 lshr_s(int64_t v, int s, int rnd) {
    if (s > 100) s = 100;
    return rnd ? ((__int128)v + ((__int128)1 << (s - 1))) >> s : (__int128)v >> s;
}
SI unsigned __int128 lshr_u(uint64_t v, int s, int rnd) {
    if (s > 100) s = 100;
    return rnd ? ((unsigned __int128)v + ((unsigned __int128)1 << (s - 1))) >> s : (unsigned __int128)v >> s;
}
SI uint64_t pmul64(uint64_t a, uint64_t b, uint64_t *hi) { // carry-less
    uint64_t lo = 0, h = 0;
    for (int i = 0; i < 64; i++)
        if ((b >> i) & 1) lo ^= a << i, h ^= i ? a >> (64 - i) : 0;
    if (hi) *hi = h;
    return lo;
}

enum {
    LOP_SQADD, LOP_UQADD, LOP_SQSUB, LOP_UQSUB, LOP_SUQADD, LOP_USQADD,
    LOP_SHL, LOP_RSHL, LOP_QSHL, LOP_QRSHL, // register shifts (b = signed byte), U selects unsigned
    LOP_SHLI, LOP_QSHLI, LOP_QSHLUI, LOP_SHRI, LOP_RSHRI, LOP_SRAI, LOP_RSRAI, LOP_SRI, LOP_SLI,
    LOP_SQDMULH, LOP_SQRDMULH, LOP_PMUL,
    LOP_SQABS, LOP_SQNEG, LOP_ABS, LOP_NEG, LOP_CLS, LOP_CLZ, LOP_CNT, LOP_RBIT, LOP_URECPE, LOP_URSQRTE,
    LOP_HADD, LOP_RHADD, LOP_HSUB, LOP_ABD, LOP_ABA,
    // long: result lanes sz+1
    LOP_ADDL, LOP_SUBL, LOP_ABDL, LOP_ABAL, LOP_MLAL, LOP_MLSL, LOP_MULL, LOP_ADDW, LOP_SUBW,
    LOP_SQDMULL, LOP_SQDMLAL, LOP_SQDMLSL, LOP_PMULL, LOP_SHLL,
    // narrow: sz = destination lane size, a/b are sz+1
    LOP_XTN, LOP_SQXTN, LOP_UQXTN, LOP_SQXTUN, LOP_SHRN, LOP_RSHRN, LOP_QSHRN, LOP_QRSHRN, LOP_QSHRUN,
    LOP_QRSHRUN, LOP_ADDHN, LOP_RADDHN, LOP_SUBHN, LOP_RSUBHN,
    LOP_SHLLI, // SSHLL/USHLL: extend, shift (long)
};
#define LOP_U 256 // unsigned variant flag

SI uint64_t lop(int op, int sz, uint64_t a, uint64_t b, uint64_t d) {
    int u = op >> 8, es = 8 << sz;
    op &= 255;
    int64_t sa = lsx(a, sz), sb = lsx(b, sz);
    __int128 xa = u ? (__int128)a : (__int128)sa, xb = u ? (__int128)b : (__int128)sb; // extended operands
    int sh = (int8_t)b;
    switch (op) {
    case LOP_SQADD: return sat_s((__int128)sa + sb, sz);
    case LOP_UQADD: return sat_u((__int128)a + b, sz);
    case LOP_SQSUB: return sat_s((__int128)sa - sb, sz);
    case LOP_UQSUB: return sat_u((__int128)a - b, sz);
    case LOP_SUQADD: return sat_s((__int128)lsx(d, sz) + a, sz);
    case LOP_USQADD: return sat_u((__int128)d + sa, sz);
    case LOP_SHL: case LOP_RSHL: case LOP_QSHL: case LOP_QRSHL: {
        int sat = op == LOP_QSHL || op == LOP_QRSHL, rnd = op == LOP_RSHL || op == LOP_QRSHL;
        if (sh >= 0) {
            if (sat) return u ? sat_uu((unsigned __int128)a << (sh > 64 ? 64 : sh), sz)
                              : sat_s(shl128(sa, sh > 64 ? 64 : sh), sz);
            return sh >= es ? 0 : a << sh;
        }
        return u ? (uint64_t)lshr_u(a, -sh, rnd) : (uint64_t)lshr_s(sa, -sh, rnd);
    }
    case LOP_SHLI: return a << b;
    case LOP_QSHLI: return u ? sat_uu((unsigned __int128)a << b, sz) : sat_s(shl128(sa, (int)b), sz);
    case LOP_QSHLUI: return sat_u(shl128(sa, (int)b), sz);
    case LOP_SHRI: return u ? (uint64_t)lshr_u(a, (int)b, 0) : (uint64_t)lshr_s(sa, (int)b, 0);
    case LOP_RSHRI: return u ? (uint64_t)lshr_u(a, (int)b, 1) : (uint64_t)lshr_s(sa, (int)b, 1);
    case LOP_SRAI: return d + (u ? (uint64_t)lshr_u(a, (int)b, 0) : (uint64_t)lshr_s(sa, (int)b, 0));
    case LOP_RSRAI: return d + (u ? (uint64_t)lshr_u(a, (int)b, 1) : (uint64_t)lshr_s(sa, (int)b, 1));
    case LOP_SRI: {
        uint64_t m = (int)b >= es ? 0 : lmask(sz) >> b;
        return (d & ~m) | ((int)b >= es ? 0 : a >> b);
    }
    case LOP_SLI: { uint64_t m = (lmask(sz) << b) & lmask(sz); return (d & ~m) | ((a << b) & m); }
    case LOP_SQDMULH: return sat_s(((__int128)sa * sb * 2) >> es, sz);
    case LOP_SQRDMULH: return sat_s(((__int128)sa * sb * 2 + ((__int128)1 << (es - 1))) >> es, sz);
    case LOP_PMUL: return pmul64(a, b, 0);
    case LOP_SQABS: return sat_s(sa < 0 ? -(__int128)sa : sa, sz);
    case LOP_SQNEG: return sat_s(-(__int128)sa, sz);
    case LOP_ABS: return sa < 0 ? (uint64_t)-sa : (uint64_t)sa;
    case LOP_NEG: return -a;
    case LOP_CLS: {
        uint64_t x = (uint64_t)(sa ^ (sa >> 1)) & lmask(sz);
        return (x ? (uint64_t)__builtin_clzll(x << (64 - es)) : (uint64_t)es) - 1;
    }
    case LOP_CLZ: return a ? (uint64_t)__builtin_clzll(a << (64 - es)) : (uint64_t)es;
    case LOP_CNT: return (uint64_t)__builtin_popcountll(a);
    case LOP_RBIT: return __builtin_bitreverse8((uint8_t)a);
    case LOP_URECPE: return (a >> 31) ? (uint64_t)(256 | simd_recip_tab[(a >> 23) & 255]) << 23 : 0xffffffffull;
    case LOP_URSQRTE: return (a >> 30) ? (uint64_t)(256 | simd_rsqrt_tab[((a >> 23) & 511) - 128]) << 23 : 0xffffffffull;
    case LOP_HADD: return (uint64_t)((xa + xb) >> 1);
    case LOP_RHADD: return (uint64_t)((xa + xb + 1) >> 1);
    case LOP_HSUB: return (uint64_t)((xa - xb) >> 1);
    case LOP_ABD: return (uint64_t)(xa > xb ? xa - xb : xb - xa);
    case LOP_ABA: return d + (uint64_t)(xa > xb ? xa - xb : xb - xa);
    case LOP_ADDL: return (uint64_t)(xa + xb);
    case LOP_SUBL: return (uint64_t)(xa - xb);
    case LOP_ABDL: return (uint64_t)(xa > xb ? xa - xb : xb - xa);
    case LOP_ABAL: return d + (uint64_t)(xa > xb ? xa - xb : xb - xa);
    case LOP_MLAL: return d + (uint64_t)(xa * xb);
    case LOP_MLSL: return d - (uint64_t)(xa * xb);
    case LOP_MULL: return (uint64_t)(xa * xb);
    case LOP_ADDW: return a + (uint64_t)xb;
    case LOP_SUBW: return a - (uint64_t)xb;
    case LOP_SQDMULL: return sat_s((__int128)sa * sb * 2, sz + 1);
    case LOP_SQDMLAL: return sat_s((__int128)lsx(d, sz + 1) + (__int128)lsx(sat_s((__int128)sa * sb * 2, sz + 1), sz + 1), sz + 1);
    case LOP_SQDMLSL: return sat_s((__int128)lsx(d, sz + 1) - (__int128)lsx(sat_s((__int128)sa * sb * 2, sz + 1), sz + 1), sz + 1);
    case LOP_PMULL: return pmul64(a, b, 0);
    case LOP_SHLL: return a << es;
    // narrowing: a (and b for xHN) are 2*es wide
    case LOP_XTN: return a;
    case LOP_SQXTN: return sat_s(lsx(a, sz + 1), sz);
    case LOP_UQXTN: return sat_u(a, sz);
    case LOP_SQXTUN: return sat_u(lsx(a, sz + 1), sz);
    case LOP_SHRN: return a >> b;
    case LOP_RSHRN: return (uint64_t)lshr_u(a, (int)b, 1);
    case LOP_QSHRN: case LOP_QRSHRN: {
        int rnd = op == LOP_QRSHRN;
        return u ? sat_u((__int128)lshr_u(a, (int)b, rnd), sz) : sat_s(lshr_s(lsx(a, sz + 1), (int)b, rnd), sz);
    }
    case LOP_QSHRUN: return sat_u(lshr_s(lsx(a, sz + 1), (int)b, 0), sz);
    case LOP_QRSHRUN: return sat_u(lshr_s(lsx(a, sz + 1), (int)b, 1), sz);
    case LOP_ADDHN: return (a + b) >> es;
    case LOP_RADDHN: return (a + b + (1ull << (es - 1))) >> es;
    case LOP_SUBHN: return (a - b) >> es;
    case LOP_RSUBHN: return (a - b + (1ull << (es - 1))) >> es;
    case LOP_SHLLI: return (uint64_t)xa << b;
    }
    return 0;
}

// Generic lane loop: ne lanes; result lane i (size szd) goes to lane offd+i of d (other lanes kept when keep,
// else zero); a = n lane offn+i (size szn), b = m lane offm+i (size szm), or lane bidx of m when bidx >= 0,
// or the constant bimm when m < 0; the accumulator is d's old lane offd+i.
SI void sl_gen(Cpu *c, int op, int sz, int ne, int d, int szd, int offd, int keep, int n, int szn, int offn, int m,
               int szm, int offm, int bidx, uint64_t bimm) {
    u128 a = c->v[n], b = m >= 0 ? c->v[m] : 0, o = c->v[d], r = keep ? o : 0;
    for (int i = 0; i < ne; i++) {
        uint64_t y = m < 0 ? bimm : lget(&b, szm, bidx >= 0 ? bidx : offm + i);
        lset(&r, szd, offd + i, lop(op, sz, lget(&a, szn, offn + i), y, lget(&o, szd, offd + i)));
    }
    c->v[d] = r;
}
// SADDLP/UADDLP/SADALP/UADALP
SI void sl_addlp(Cpu *c, int u, int acc, int Q, int sz, int d, int n) {
    u128 a = c->v[n], o = c->v[d], r = 0;
    for (int i = 0; i < (4 << Q) >> sz; i++) {
        uint64_t x = lget(&a, sz, 2 * i), y = lget(&a, sz, 2 * i + 1);
        uint64_t s = u ? x + y : (uint64_t)(lsx(x, sz) + lsx(y, sz));
        lset(&r, sz + 1, i, s + (acc ? lget(&o, sz + 1, i) : 0));
    }
    c->v[d] = r;
}
// ADDV, SADDLV/UADDLV, SMAXV/UMAXV, SMINV/UMINV (op 0..3; u = unsigned)
SI void sl_across(Cpu *c, int op, int u, int Q, int sz, int d, int n) {
    u128 a = c->v[n];
    int ne = (8 << Q) >> sz;
    int64_t acc = u ? (int64_t)lget(&a, sz, 0) : lsx(lget(&a, sz, 0), sz);
    for (int i = 1; i < ne; i++) {
        int64_t x = u ? (int64_t)lget(&a, sz, i) : lsx(lget(&a, sz, i), sz);
        if (op <= 1) acc += x;
        else if (op == 2) acc = u ? ((uint64_t)x > (uint64_t)acc ? x : acc) : (x > acc ? x : acc);
        else acc = u ? ((uint64_t)x < (uint64_t)acc ? x : acc) : (x < acc ? x : acc);
    }
    c->v[d] = (uint64_t)acc & lmask(op == 1 ? sz + 1 : sz);
}
// PMULL2/PMULL 1Q (64x64 -> 128 carry-less)
SI void sl_pmull64(Cpu *c, int part, int d, int n, int m) {
    uint64_t hi, lo = pmul64(((sa64 *)&c->v[n])[part], ((sa64 *)&c->v[m])[part], &hi);
    c->v[d] = ((u128)hi << 64) | lo;
}

// ---- FP vector helpers: lanes of the 64-bit form are computed on a zero upper half
SI vf32 vsplat_s(vf32 v, int i, int Q) { float x = v[i]; return Q ? (vf32){x, x, x, x} : (vf32){x, x, 0, 0}; }
SI vf64 vsplat_d(vf64 v, int i, int Q) { double x = v[i]; (void)Q; return (vf64){x, x}; }
#define SV_FOP_S(op, Q, d, n, m, a) VW(vf32, Q, d, vop_s(c, op, n, m, a))
#define SV_FOP_D(op, Q, d, n, m, a) VW(vf64, Q, d, vop_d(c, op, n, m, a))
SI vf32 vneg_s(vf32 v) { return (vf32)((vu32)v ^ 0x80000000u); }
SI vf64 vneg_d(vf64 v) { return (vf64)((vu64)v ^ 0x8000000000000000ull); }
SI float pair_s(Cpu *c, int op, float a, float b) { return op_s(c, op, a, b, 0); }
SI double pair_d(Cpu *c, int op, double a, double b) { return op_d(c, op, a, b, 0); }
// FMAXV/FMINV/FMAXNMV/FMINNMV (4S): pairwise tree
SI void sv_fred_s(Cpu *c, int op, int d, int n) {
    vf32 v = VREG(vf32, n);
    c->v[d] = fb_s(pair_s(c, op, pair_s(c, op, v[0], v[1]), pair_s(c, op, v[2], v[3])));
}
// lane-wise FP conversions
SI vu32 vtoint_s(Cpu *c, vf32 v, int rm, int fbits, int uns) {
    vu32 r;
    for (int i = 0; i < 4; i++) r[i] = (uint32_t)toint_s(c, v[i], rm, fbits, 32, uns);
    return r;
}
SI vu64 vtoint_d(Cpu *c, vf64 v, int rm, int fbits, int uns) {
    vu64 r;
    for (int i = 0; i < 2; i++) r[i] = toint_d(c, v[i], rm, fbits, 64, uns);
    return r;
}
SI vf32 vfromint_s(Cpu *c, vu32 v, int fbits, int uns) {
    vf32 r;
    if (!(c->fpcr & FPCR_RM)) {
        r = uns ? CVT(v, vf32) : CVT((vs32)v, vf32);
        if (fbits) r = r / (float)(1ull << (fbits - 1)) / 2.0f;
        return r;
    }
    for (int i = 0; i < 4; i++) r[i] = fromint_s(c, v[i], 32, uns, fbits);
    return r;
}
SI vf64 vfromint_d(Cpu *c, vu64 v, int fbits, int uns) {
    vf64 r;
    for (int i = 0; i < 2; i++) r[i] = fromint_d(c, v[i], 64, uns, fbits);
    return r;
}
SI vf32 vrecpe_s(Cpu *c, vf32 v) { for (int i = 0; i < 4; i++) v[i] = recpe_s(c, v[i]); return v; }
SI vf64 vrecpe_d(Cpu *c, vf64 v) { for (int i = 0; i < 2; i++) v[i] = recpe_d(c, v[i]); return v; }
SI vf32 vrsqrte_s(Cpu *c, vf32 v) { for (int i = 0; i < 4; i++) v[i] = rsqrte_s(c, v[i]); return v; }
SI vf64 vrsqrte_d(Cpu *c, vf64 v) { for (int i = 0; i < 2; i++) v[i] = rsqrte_d(c, v[i]); return v; }

// FCVTL/FCVTL2 (h->s, s->d), FCVTN/FCVTN2 (s->h, d->s), FCVTXN/FCVTXN2
SI void sv_fcvtl(Cpu *c, int sz, int part, int d, int n) {
    u128 a = c->v[n], r = 0;
    if (sz == 0) {
        vf32 o;
        for (int i = 0; i < 4; i++) o[i] = simd_h2s(c, (uint16_t)lget(&a, 1, 4 * part + i));
        r = (u128)o;
    } else {
        vf64 o;
        for (int i = 0; i < 2; i++) o[i] = simd_s2d(c, fv_s((uint32_t)lget(&a, 2, 2 * part + i)));
        r = (u128)o;
    }
    c->v[d] = r;
}
SI void sv_fcvtn(Cpu *c, int sz, int part, int odd, int d, int n) {
    u128 a = c->v[n], r = part ? (uint64_t)c->v[d] : 0;
    if (sz == 0) {
        for (int i = 0; i < 4; i++) lset(&r, 1, 4 * part + i, simd_s2h(c, fv_s((uint32_t)lget(&a, 2, i))));
    } else {
        for (int i = 0; i < 2; i++) {
            double x = fv_d(lget(&a, 3, i));
            lset(&r, 2, 2 * part + i, fb_s(odd ? simd_d2s_odd(c, x) : simd_d2s(c, x)));
        }
    }
    c->v[d] = r;
}

// TBL / TBX with len+1 table registers
SI void sv_tbl(Cpu *c, int Q, int len, int tbx, int d, int n, int m) {
    uint8_t tab[64];
    for (int i = 0; i <= len; i++) memcpy(tab + 16 * i, &c->v[(n + i) & 31], 16);
    u128 idx = c->v[m], o = c->v[d], r = 0;
    for (int i = 0; i < 8 << Q; i++) {
        unsigned k = (unsigned)lget(&idx, 0, i);
        lset(&r, 0, i, k < 16u * (len + 1) ? tab[k] : tbx ? lget(&o, 0, i) : 0);
    }
    c->v[d] = r;
}

// ---- CRC32 / CRC32C (bytes = 1, 2, 4, 8)
SI uint32_t simd_crc(const uint32_t *t, uint32_t acc, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) acc = t[(acc ^ (uint8_t)(v >> (8 * i))) & 0xff] ^ (acc >> 8);
    return acc;
}
#define CRC32(acc, v, n) simd_crc(simd_crc32_tab, (uint32_t)(acc), (v), n)
#define CRC32C(acc, v, n) simd_crc(simd_crc32c_tab, (uint32_t)(acc), (v), n)

// ---- crypto (AES, SHA1, SHA256)
SI uint8_t aes_xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
SI uint8_t aes_mul(uint8_t x, int k) { // k in {9, 11, 13, 14}
    uint8_t x2 = aes_xt(x), x4 = aes_xt(x2), x8 = aes_xt(x4);
    return (uint8_t)(x8 ^ (k & 4 ? x4 : 0) ^ (k & 2 ? x2 : 0) ^ (k & 1 ? x : 0));
}
SI void sv_aes(Cpu *c, int op, int d, int n) { // 0 AESE, 1 AESD, 2 AESMC, 3 AESIMC
    uint8_t s[16], r[16];
    u128 v = op < 2 ? c->v[d] ^ c->v[n] : c->v[n];
    memcpy(s, &v, 16);
    if (op == 0) {
        for (int i = 0; i < 16; i++) r[i] = simd_aes_sbox[s[(i + 4 * (i & 3)) & 15]];
    } else if (op == 1) {
        for (int i = 0; i < 16; i++) r[i] = simd_aes_isbox[s[(i - 4 * (i & 3)) & 15]];
    } else {
        for (int col = 0; col < 4; col++) {
            uint8_t *a = s + 4 * col, *o = r + 4 * col;
            for (int i = 0; i < 4; i++) {
                uint8_t a0 = a[i], a1 = a[(i + 1) & 3], a2 = a[(i + 2) & 3], a3 = a[(i + 3) & 3];
                o[i] = op == 2 ? (uint8_t)(aes_xt(a0) ^ aes_xt(a1) ^ a1 ^ a2 ^ a3)
                               : (uint8_t)(aes_mul(a0, 14) ^ aes_mul(a1, 11) ^ aes_mul(a2, 13) ^ aes_mul(a3, 9));
            }
        }
    }
    memcpy(&v, r, 16);
    c->v[d] = v;
}
SI uint32_t rol32_(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
SI uint32_t ror32_(uint32_t x, int s) { return (x >> s) | (x << (32 - s)); }
SI void sv_sha1hash(Cpu *c, int op, int d, int n, int m) { // 0 SHA1C, 1 SHA1P, 2 SHA1M
    uint32_t x[4], w[4], y = (uint32_t)c->v[n];
    memcpy(x, &c->v[d], 16);
    memcpy(w, &c->v[m], 16);
    for (int e = 0; e < 4; e++) {
        uint32_t t = op == 0 ? (((x[2] ^ x[3]) & x[1]) ^ x[3]) : op == 1 ? (x[1] ^ x[2] ^ x[3])
                                                               : ((x[1] & x[2]) | ((x[1] | x[2]) & x[3]));
        y = y + rol32_(x[0], 5) + t + w[e];
        x[1] = rol32_(x[1], 30);
        uint32_t top = x[3];
        x[3] = x[2], x[2] = x[1], x[1] = x[0], x[0] = y, y = top;
    }
    memcpy(&c->v[d], x, 16);
}
SI void sv_sha256hash(Cpu *c, int part1, int d, int n, int m) { // SHA256H (part1), SHA256H2
    uint32_t x[4], y[4], w[4];
    memcpy(part1 ? x : y, &c->v[d], 16);
    memcpy(part1 ? y : x, &c->v[n], 16);
    memcpy(w, &c->v[m], 16);
    for (int e = 0; e < 4; e++) {
        uint32_t chs = ((y[1] ^ y[2]) & y[0]) ^ y[2];
        uint32_t maj = (x[0] & x[1]) | ((x[0] | x[1]) & x[2]);
        uint32_t t1 = y[3] + (ror32_(y[0], 6) ^ ror32_(y[0], 11) ^ ror32_(y[0], 25)) + chs + w[e];
        x[3] = t1 + x[3];
        y[3] = t1 + (ror32_(x[0], 2) ^ ror32_(x[0], 13) ^ ror32_(x[0], 22)) + maj;
        uint32_t t = y[3];
        y[3] = y[2], y[2] = y[1], y[1] = y[0], y[0] = x[3];
        x[3] = x[2], x[2] = x[1], x[1] = x[0], x[0] = t;
    }
    memcpy(&c->v[d], part1 ? x : y, 16);
}
SI void sv_sha_misc(Cpu *c, int op, int d, int n, int m) { // 0 SHA1SU0, 1 SHA1SU1, 2 SHA256SU0, 3 SHA256SU1, 4 SHA1H
    uint32_t a[4], b[4], k[4], r[4], t[4];
    memcpy(a, &c->v[d], 16);
    memcpy(b, &c->v[n], 16);
    memcpy(k, &c->v[m], 16);
    switch (op) {
    case 0:
        t[0] = a[2], t[1] = a[3], t[2] = b[0], t[3] = b[1];
        for (int i = 0; i < 4; i++) r[i] = t[i] ^ a[i] ^ k[i];
        break;
    case 1:
        for (int i = 0; i < 4; i++) t[i] = a[i] ^ (i < 3 ? b[i + 1] : 0);
        for (int i = 0; i < 4; i++) r[i] = rol32_(t[i], 1);
        r[3] ^= rol32_(t[0], 2);
        break;
    case 2:
        t[0] = a[1], t[1] = a[2], t[2] = a[3], t[3] = b[0];
        for (int i = 0; i < 4; i++) r[i] = (ror32_(t[i], 7) ^ ror32_(t[i], 18) ^ (t[i] >> 3)) + a[i];
        break;
    case 3: {
        uint32_t t0[4] = {b[1], b[2], b[3], k[0]};
        for (int e = 0; e < 4; e++) {
            uint32_t x = e < 2 ? k[e + 2] : r[e - 2];
            r[e] = (ror32_(x, 17) ^ ror32_(x, 19) ^ (x >> 10)) + a[e] + t0[e];
        }
        break;
    }
    default:
        c->v[d] = rol32_(b[0], 30);
        return;
    }
    memcpy(&c->v[d], r, 16);
}

// ---- loads / stores of lanes
SI uint64_t simd_ld(uint64_t a, int sz) {
    switch (sz) {
    case 0: return LD8(a);
    case 1: return LD16(a);
    case 2: return LD32(a);
    default: return LD64(a);
    }
}
SI void simd_st(uint64_t a, int sz, uint64_t v) {
    switch (sz) {
    case 0: ST8(a, v); break;
    case 1: ST16(a, v); break;
    case 2: ST32(a, v); break;
    default: ST64(a, v); break;
    }
}
// LDn/STn (multiple structures): selem interleaved registers, rpt repeats (LD1 with 2-4 registers)
SI void sl_ldm(Cpu *c, int Q, int sz, int selem, int rpt, int t, uint64_t ea) {
    u128 r[4] = {0, 0, 0, 0};
    int ne = (8 << Q) >> sz;
    for (int k = 0; k < rpt; k++)
        for (int e = 0; e < ne; e++)
            for (int s = 0; s < selem; s++, ea += 1 << sz) lset(&r[k + s], sz, e, simd_ld(ea, sz));
    for (int i = 0; i < rpt + selem - 1; i++) c->v[(t + i) & 31] = r[i];
}
SI void sl_stm(Cpu *c, int Q, int sz, int selem, int rpt, int t, uint64_t ea) {
    u128 r[4];
    int ne = (8 << Q) >> sz;
    for (int i = 0; i < rpt + selem - 1; i++) r[i] = c->v[(t + i) & 31];
    for (int k = 0; k < rpt; k++)
        for (int e = 0; e < ne; e++)
            for (int s = 0; s < selem; s++, ea += 1 << sz) simd_st(ea, sz, lget(&r[k + s], sz, e));
}
// single structure: lane idx of selem registers; LDnR replicates into every lane
SI void sl_lds(Cpu *c, int sz, int selem, int idx, int t, uint64_t ea) {
    for (int s = 0; s < selem; s++) {
        u128 v = c->v[(t + s) & 31];
        lset(&v, sz, idx, simd_ld(ea + (s << sz), sz));
        c->v[(t + s) & 31] = v;
    }
}
SI void sl_sts(Cpu *c, int sz, int selem, int idx, int t, uint64_t ea) {
    for (int s = 0; s < selem; s++) simd_st(ea + (s << sz), sz, lget(&c->v[(t + s) & 31], sz, idx));
}
SI void sl_ldr(Cpu *c, int Q, int sz, int selem, int t, uint64_t ea) {
    uint64_t x[4];
    for (int s = 0; s < selem; s++) x[s] = simd_ld(ea + (s << sz), sz);
    for (int s = 0; s < selem; s++) {
        u128 v = 0;
        for (int e = 0; e < (8 << Q) >> sz; e++) lset(&v, sz, e, x[s]);
        c->v[(t + s) & 31] = v;
    }
}
