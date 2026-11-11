// SHA-256 and ECDSA P-256 verification (runtime/update_crypto.h).
#include "update_crypto.h"
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ SHA-256
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

void uc_sha256_init(UcSha256 *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->total = 0;
    c->used = 0;
}

void uc_sha256_update(UcSha256 *c, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    c->total += n;
    while (n) {
        if (!c->used && n >= 64) {
            sha_block(c->h, p);
            p += 64, n -= 64;
            continue;
        }
        size_t k = 64 - c->used < n ? 64 - c->used : n;
        memcpy(c->buf + c->used, p, k);
        c->used += k, p += k, n -= k;
        if (c->used == 64) {
            sha_block(c->h, c->buf);
            c->used = 0;
        }
    }
}

void uc_sha256_final(UcSha256 *c, uint8_t out[32]) {
    uint64_t bits = c->total * 8;
    static const uint8_t one = 0x80, zero = 0;
    uc_sha256_update(c, &one, 1);
    while (c->used != 56) uc_sha256_update(c, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - i * 8));
    uc_sha256_update(c, len, 8);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(c->h[i / 4] >> (24 - (i % 4) * 8));
}

bool uc_sha256_file(const char *path, uint8_t out[32]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static __thread uint8_t buf[1 << 16];
    UcSha256 c;
    uc_sha256_init(&c);
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) uc_sha256_update(&c, buf, n);
    bool ok = !ferror(f);
    fclose(f);
    if (ok) uc_sha256_final(&c, out);
    return ok;
}

// ------------------------------------------------------------------ P-256
// Plain and slow on purpose: a modular product is 256 doublings and additions. A verification takes well under a second
// and happens once per update, and this is far easier to get right than a fast reduction.
typedef struct { uint32_t v[8]; } U;  // little-endian limbs

static const U P = {{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0, 1, 0xFFFFFFFF}};
static const U N = {{0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0xFFFFFFFF}};
static const U B = {{0x27D2604B, 0x3BCE3C3E, 0xCC53B0F6, 0x651D06B0, 0x769886BC, 0xB3EBBD55, 0xAA3A93E7, 0x5AC635D8}};
static const U GX = {{0xD898C296, 0xF4A13945, 0x2DEB33A0, 0x77037D81, 0x63A440F2, 0xF8BCE6E5, 0xE12C4247, 0x6B17D1F2}};
static const U GY = {{0x37BF51F5, 0xCBB64068, 0x6B315ECE, 0x2BCE3357, 0x7C0F9E16, 0x8EE7EB4A, 0xFE1A7F9B, 0x4FE342E2}};
static const U ONE = {{1, 0, 0, 0, 0, 0, 0, 0}};

static int u_cmp(const U *a, const U *b) {
    for (int i = 7; i >= 0; i--)
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    return 0;
}
static bool u_zero(const U *a) {
    uint32_t o = 0;
    for (int i = 0; i < 8; i++) o |= a->v[i];
    return !o;
}
static uint32_t u_add(U *r, const U *a, const U *b) {
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) {
        c += (uint64_t)a->v[i] + b->v[i];
        r->v[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}
static uint32_t u_sub(U *r, const U *a, const U *b) {
    int64_t c = 0;
    for (int i = 0; i < 8; i++) {
        c += (int64_t)a->v[i] - b->v[i];
        r->v[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)(c & 1);
}
static void add_mod(U *r, const U *a, const U *b, const U *m) {
    U t;
    uint32_t carry = u_add(&t, a, b);
    if (carry || u_cmp(&t, m) >= 0) u_sub(&t, &t, m);
    *r = t;
}
static void sub_mod(U *r, const U *a, const U *b, const U *m) {
    U t;
    if (u_sub(&t, a, b)) u_add(&t, &t, m);
    *r = t;
}
static void mul_mod(U *r, const U *a, const U *b, const U *m) {
    U t = {{0}};
    for (int i = 255; i >= 0; i--) {
        add_mod(&t, &t, &t, m);
        if (b->v[i / 32] >> (i % 32) & 1) add_mod(&t, &t, a, m);
    }
    *r = t;
}
static void inv_mod(U *r, const U *a, const U *m) {  // (m is prime: a^(m-2))
    U e, t = ONE, two = {{2, 0, 0, 0, 0, 0, 0, 0}};
    u_sub(&e, m, &two);
    for (int i = 255; i >= 0; i--) {
        mul_mod(&t, &t, &t, m);
        if (e.v[i / 32] >> (i % 32) & 1) mul_mod(&t, &t, a, m);
    }
    *r = t;
}

static void u_from_be(U *r, const uint8_t b[32]) {
    for (int i = 0; i < 8; i++) {
        const uint8_t *p = b + 28 - i * 4;
        r->v[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
}

typedef struct { U x, y, z; } Pt;  // Jacobian; z == 0: the point at infinity

#define MUL(r, a, b) mul_mod(&(r), &(a), &(b), &P)
#define ADD(r, a, b) add_mod(&(r), &(a), &(b), &P)
#define SUB(r, a, b) sub_mod(&(r), &(a), &(b), &P)

static void pt_double(Pt *r, const Pt *a) {
    if (u_zero(&a->z) || u_zero(&a->y)) {
        memset(r, 0, sizeof *r);
        return;
    }
    U delta, gamma, beta, alpha, t1, t2, x3, y3, z3;
    MUL(delta, a->z, a->z);
    MUL(gamma, a->y, a->y);
    MUL(beta, a->x, gamma);
    SUB(t1, a->x, delta);
    ADD(t2, a->x, delta);
    MUL(alpha, t1, t2);
    ADD(t1, alpha, alpha);
    ADD(alpha, t1, alpha);  // 3 (x - delta)(x + delta), as a = -3
    MUL(x3, alpha, alpha);
    ADD(t1, beta, beta);
    ADD(t1, t1, t1);        // 4 beta
    ADD(t2, t1, t1);        // 8 beta
    SUB(x3, x3, t2);
    ADD(t2, a->y, a->z);
    MUL(z3, t2, t2);
    SUB(z3, z3, gamma);
    SUB(z3, z3, delta);
    SUB(t1, t1, x3);
    MUL(y3, alpha, t1);
    MUL(t2, gamma, gamma);
    ADD(t2, t2, t2);
    ADD(t2, t2, t2);
    ADD(t2, t2, t2);        // 8 gamma^2
    SUB(y3, y3, t2);
    r->x = x3, r->y = y3, r->z = z3;
}

static void pt_add(Pt *r, const Pt *a, const Pt *b) {
    if (u_zero(&a->z)) { *r = *b; return; }
    if (u_zero(&b->z)) { *r = *a; return; }
    U z1z1, z2z2, u1, u2, s1, s2, h, rr, hh, hhh, v, t, x3, y3, z3;
    MUL(z1z1, a->z, a->z);
    MUL(z2z2, b->z, b->z);
    MUL(u1, a->x, z2z2);
    MUL(u2, b->x, z1z1);
    MUL(t, b->z, z2z2);
    MUL(s1, a->y, t);
    MUL(t, a->z, z1z1);
    MUL(s2, b->y, t);
    SUB(h, u2, u1);
    SUB(rr, s2, s1);
    if (u_zero(&h)) {
        if (u_zero(&rr)) pt_double(r, a);
        else memset(r, 0, sizeof *r);
        return;
    }
    MUL(hh, h, h);
    MUL(hhh, h, hh);
    MUL(v, u1, hh);
    MUL(x3, rr, rr);
    SUB(x3, x3, hhh);
    SUB(x3, x3, v);
    SUB(x3, x3, v);
    SUB(t, v, x3);
    MUL(y3, rr, t);
    MUL(t, s1, hhh);
    SUB(y3, y3, t);
    MUL(t, a->z, b->z);
    MUL(z3, t, h);
    r->x = x3, r->y = y3, r->z = z3;
}

static void pt_mul(Pt *r, const Pt *a, const U *k) {
    Pt t;
    memset(&t, 0, sizeof t);
    for (int i = 255; i >= 0; i--) {
        pt_double(&t, &t);
        if (k->v[i / 32] >> (i % 32) & 1) pt_add(&t, &t, a);
    }
    *r = t;
}

static bool on_curve(const U *x, const U *y) {  // y^2 = x^3 - 3x + b
    U left, right, t;
    MUL(left, *y, *y);
    MUL(t, *x, *x);
    MUL(right, t, *x);
    ADD(t, *x, *x);
    ADD(t, t, *x);
    SUB(right, right, t);
    ADD(right, right, B);
    return !u_cmp(&left, &right);
}

bool uc_p256_verify(const uint8_t hash[32], const uint8_t sig[64], const uint8_t pub[64]) {
    U r, s, e, qx, qy;
    u_from_be(&r, sig);
    u_from_be(&s, sig + 32);
    u_from_be(&e, hash);
    u_from_be(&qx, pub);
    u_from_be(&qy, pub + 32);
    if (u_zero(&r) || u_zero(&s) || u_cmp(&r, &N) >= 0 || u_cmp(&s, &N) >= 0) return false;
    if (u_cmp(&qx, &P) >= 0 || u_cmp(&qy, &P) >= 0 || !on_curve(&qx, &qy)) return false;
    if (u_cmp(&e, &N) >= 0) u_sub(&e, &e, &N);
    U w, u1, u2;
    inv_mod(&w, &s, &N);
    mul_mod(&u1, &e, &w, &N);
    mul_mod(&u2, &r, &w, &N);
    Pt g = {GX, GY, ONE}, q = {qx, qy, ONE}, a, b, sum;
    pt_mul(&a, &g, &u1);
    pt_mul(&b, &q, &u2);
    pt_add(&sum, &a, &b);
    if (u_zero(&sum.z)) return false;
    U zi, zi2, x;
    inv_mod(&zi, &sum.z, &P);
    MUL(zi2, zi, zi);
    MUL(x, sum.x, zi2);
    if (u_cmp(&x, &N) >= 0) u_sub(&x, &x, &N);
    return !u_cmp(&x, &r);
}
