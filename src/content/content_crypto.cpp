#include "content_crypto.h"

#include <string.h>

#if !CONTENT_PORTABLE_SHA512
#include <mbedtls/version.h>

// mbedtls renamed these between major versions: 2.x has the int-returning
// _ret suffix (the void-returning originals are deprecated and may be
// compiled out), 3.x dropped the suffix and returns int from the plain names.
// ESP-IDF ships 2.28 today, so both spellings have to be reachable or this
// file breaks the day the platform pin moves.
#if MBEDTLS_VERSION_MAJOR >= 3
#define HH_SHA512_STARTS(c, is384)      mbedtls_sha512_starts((c), (is384))
#define HH_SHA512_UPDATE(c, buf, len)   mbedtls_sha512_update((c), (buf), (len))
#define HH_SHA512_FINISH(c, out)        mbedtls_sha512_finish((c), (out))
#else
#define HH_SHA512_STARTS(c, is384)      mbedtls_sha512_starts_ret((c), (is384))
#define HH_SHA512_UPDATE(c, buf, len)   mbedtls_sha512_update_ret((c), (buf), (len))
#define HH_SHA512_FINISH(c, out)        mbedtls_sha512_finish_ret((c), (out))
#endif
#endif

// ── HexHound - Content Pack Crypto Implementation ────────────────

namespace {

// ══ SHA-512 ═══════════════════════════════════════════════════════════════

#if CONTENT_PORTABLE_SHA512

// FIPS 180-4 SHA-512. Only compiled off-target, and deliberately the plain
// textbook construction for the same reason hexpass_crypto's SHA-256 is: this
// code exists to be obviously checkable against published vectors.
//
// The round constants are the first 64 bits of the fractional parts of the
// cube roots of the first 80 primes, and the initial state the same of the
// square roots of the first 8. They were regenerated from that definition and
// the resulting table checked against Python's hashlib on messages either
// side of every block boundary before being pasted here, rather than being
// copied out of another implementation.

const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
    0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
    0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
    0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
    0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

inline uint64_t ror64(uint64_t x, unsigned n) {
    return (x >> n) | (x << (64 - n));
}

void sha512Compress(ContentCrypto::Sha512Ctx& s, const uint8_t* block) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int b = 0; b < 8; b++) {
            w[i] = (w[i] << 8) | (uint64_t)block[i * 8 + b];
        }
    }
    for (int i = 16; i < 80; i++) {
        const uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint64_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3];
    uint64_t e = s.h[4], f = s.h[5], g = s.h[6], hh = s.h[7];

    for (int i = 0; i < 80; i++) {
        const uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        const uint64_t ch = (e & f) ^ ((~e) & g);
        const uint64_t t1 = hh + S1 + ch + K512[i] + w[i];
        const uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint64_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d;
    s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += hh;
}

void sha512InitPortable(ContentCrypto::Sha512Ctx& s) {
    s.h[0] = 0x6a09e667f3bcc908ULL; s.h[1] = 0xbb67ae8584caa73bULL;
    s.h[2] = 0x3c6ef372fe94f82bULL; s.h[3] = 0xa54ff53a5f1d36f1ULL;
    s.h[4] = 0x510e527fade682d1ULL; s.h[5] = 0x9b05688c2b3e6c1fULL;
    s.h[6] = 0x1f83d9abfb41bd6bULL; s.h[7] = 0x5be0cd19137e2179ULL;
    s.byteLen = 0;
    s.bufLen  = 0;
}

void sha512UpdatePortable(ContentCrypto::Sha512Ctx& s, const uint8_t* data, size_t len) {
    if (data == nullptr) return;
    s.byteLen += (uint64_t)len;
    while (len > 0) {
        const size_t take = (128 - s.bufLen < len) ? (128 - s.bufLen) : len;
        memcpy(s.buf + s.bufLen, data, take);
        s.bufLen += take;
        data     += take;
        len      -= take;
        if (s.bufLen == 128) {
            sha512Compress(s, s.buf);
            s.bufLen = 0;
        }
    }
}

void sha512FinalPortable(ContentCrypto::Sha512Ctx& s, uint8_t out[64]) {
    // The length field is 128 bits. Callers here hash a few kilobytes of pack
    // or a couple of megabytes of firmware image, so the high half is always
    // zero; it is written out rather than assumed.
    const uint64_t bits = s.byteLen * 8u;

    uint8_t pad = 0x80;
    sha512UpdatePortable(s, &pad, 1);
    pad = 0x00;
    while (s.bufLen != 112) {
        sha512UpdatePortable(s, &pad, 1);
    }
    uint8_t lenBytes[16];
    memset(lenBytes, 0, 8);
    for (int i = 0; i < 8; i++) {
        lenBytes[8 + i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha512UpdatePortable(s, lenBytes, 16);

    for (int i = 0; i < 8; i++) {
        for (int b = 0; b < 8; b++) {
            out[i * 8 + b] = (uint8_t)(s.h[i] >> (56 - b * 8));
        }
    }
}

#endif  // CONTENT_PORTABLE_SHA512

// ══ Ed25519 ═══════════════════════════════════════════════════════════════
//
// Field elements are 16 limbs of radix 2^16 held in int64, so a limb can go
// negative and a product of two limbs cannot overflow before the carry pass.
// This is the classic compact representation; it is chosen over ref10's
// radix-2^25.5 because the schoolbook loop below is short enough to read in
// one sitting, and verification runs twice at boot and never again.
//
// Nothing here is constant time and nothing here needs to be. Every value a
// verifier touches is public: the pack, the signature, the public key.

typedef int64_t gf[16];

const gf GF0 = {0};
const gf GF1 = {1};

// d = -121665/121666 mod 2^255-19
//   = 0x52036cee2b6ffe738cc740797779e89800700a4d4141d8ab75eb4dca135978a3
// split into 16-bit little-endian limbs.
const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203
};

// Base point.
// x = 0x216936d3cd6e53fec0a4e231fdd6dc5c692cc7609525a7b2c9562d608f25d51a
const gf BX = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169
};
// y = 4/5 = 0x6666666666666666666666666666666666666666666666666666666666666658
const gf BY = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666
};
// sqrt(-1) = 0x2b8324804fc1df0b2b4d00993dfbd7a72f431806ad2fe478c4ee1b274a0ea0b0
const gf SQRTM1 = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83
};

// Group order L = 2^252 + 27742317777372353535851937790883648493, little-endian.
const int64_t ORDER_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0x10
};

// 2^255 - 19, little-endian, for the canonical-encoding checks.
const uint8_t FIELD_P[32] = {
    0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
};

void gfSet(gf r, const gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

void gfAdd(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

void gfSub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

// Carry propagation with the 2^256 = 38 wrap folded back into limb 0. The
// +2^16 / -1 dance is what makes it correct for negative limbs too.
void gfCarry(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        const int64_t c = o[i] >> 16;
        if (i < 15) {
            o[i + 1] += c - 1;
        } else {
            o[0] += 38 * (c - 1);
        }
        o[i] -= c << 16;
    }
}

void gfMul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) {
        const int64_t ai = a[i];
        for (int j = 0; j < 16; j++) t[i + j] += ai * b[j];
    }
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    gfCarry(o);
    gfCarry(o);
}

void gfSquare(gf o, const gf a) { gfMul(o, a, a); }

// Conditional swap. Used only inside the final reduction in gfPack.
void gfSwap(gf p, gf q, int64_t b) {
    const int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        const int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

// Fully reduce and serialise. Two conditional subtractions of p are enough
// because gfCarry has already brought the value under 2p.
void gfPack(uint8_t out[32], const gf n) {
    gf t, m;
    gfSet(t, n);
    gfCarry(t);
    gfCarry(t);
    gfCarry(t);
    for (int pass = 0; pass < 2; pass++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int64_t borrow = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        gfSwap(t, m, 1 - borrow);
    }
    for (int i = 0; i < 16; i++) {
        out[2 * i]     = (uint8_t)(t[i] & 0xff);
        out[2 * i + 1] = (uint8_t)((t[i] >> 8) & 0xff);
    }
}

void gfUnpack(gf o, const uint8_t in[32]) {
    for (int i = 0; i < 16; i++) {
        o[i] = (int64_t)in[2 * i] + ((int64_t)in[2 * i + 1] << 8);
    }
    o[15] &= 0x7fff;   // the top bit is the x sign, not part of y
}

bool gfEqual(const gf a, const gf b) {
    uint8_t pa[32], pb[32];
    gfPack(pa, a);
    gfPack(pb, b);
    return memcmp(pa, pb, 32) == 0;
}

int gfParity(const gf a) {
    uint8_t p[32];
    gfPack(p, a);
    return p[0] & 1;
}

void gfInvert(gf o, const gf i) {
    gf c;
    gfSet(c, i);
    for (int a = 253; a >= 0; a--) {
        gfSquare(c, c);
        if (a != 2 && a != 4) gfMul(c, c, i);
    }
    gfSet(o, c);
}

// x^((p-5)/8), the exponent the square-root recovery needs.
void gfPow2523(gf o, const gf i) {
    gf c;
    gfSet(c, i);
    for (int a = 250; a >= 0; a--) {
        gfSquare(c, c);
        if (a != 1) gfMul(c, c, i);
    }
    gfSet(o, c);
}

// ── Group arithmetic, extended coordinates (X : Y : Z : T) ────────────────

// Unified twisted-Edwards addition. Correct for p == q, which is why the
// ladder below needs no separate doubling routine. Every read of q happens
// before the first write to p, so add(p, p) is safe.
void edAdd(gf p[4], gf q[4]) {
    gf a, b, c, d, t;

    gfSub(a, p[1], p[0]);          // Y1 - X1
    gfSub(t, q[1], q[0]);          // Y2 - X2
    gfMul(a, a, t);                // A
    gfAdd(b, p[0], p[1]);          // Y1 + X1
    gfAdd(t, q[0], q[1]);          // Y2 + X2
    gfMul(b, b, t);                // B
    gfMul(c, p[3], q[3]);          // T1 * T2
    gfMul(c, c, D);
    gfAdd(c, c, c);                // C = 2d * T1 * T2
    gfMul(d, p[2], q[2]);
    gfAdd(d, d, d);                // D = 2 * Z1 * Z2

    gfSub(t, b, a);                // E = B - A
    gfAdd(b, b, a);                // H = B + A
    gfSub(a, d, c);                // F = D - C
    gfAdd(c, d, c);                // G = D + C

    gfMul(p[0], t, a);             // X3 = E * F
    gfMul(p[1], b, c);             // Y3 = H * G
    gfMul(p[2], c, a);             // Z3 = G * F
    gfMul(p[3], t, b);             // T3 = E * H
}

void edIdentity(gf p[4]) {
    gfSet(p[0], GF0);
    gfSet(p[1], GF1);
    gfSet(p[2], GF1);
    gfSet(p[3], GF0);
}

// r = [s]q, plain double-and-add from the top bit down. The branch on the
// scalar bit is a deliberate choice: s is public in a verifier, and a ladder
// would double the cost for no security this side of the transaction.
void edScalarMul(gf r[4], gf q[4], const uint8_t s[32]) {
    edIdentity(r);
    for (int i = 255; i >= 0; i--) {
        edAdd(r, r);
        if ((s[i >> 3] >> (i & 7)) & 1) edAdd(r, q);
    }
}

void edScalarBase(gf r[4], const uint8_t s[32]) {
    gf b[4];
    gfSet(b[0], BX);
    gfSet(b[1], BY);
    gfSet(b[2], GF1);
    gfMul(b[3], BX, BY);
    edScalarMul(r, b, s);
}

void edPack(uint8_t out[32], gf p[4]) {
    gf zi, tx, ty;
    gfInvert(zi, p[2]);
    gfMul(tx, p[0], zi);
    gfMul(ty, p[1], zi);
    gfPack(out, ty);
    out[31] ^= (uint8_t)(gfParity(tx) << 7);
}

// Decompresses the public key AND negates it, because the verification
// equation is [S]B - [h]A = R and folding the sign in here means the ladder
// never has to subtract.
//
// Returns false when the encoded y has no matching x, which is the case for
// most 32-byte strings: an attacker cannot supply an arbitrary "key".
bool edUnpackNeg(gf r[4], const uint8_t pk[32]) {
    gf num, den, den2, den4, den6, t, chk;

    gfSet(r[2], GF1);
    gfUnpack(r[1], pk);

    gfSquare(num, r[1]);           // y^2
    gfMul(den, num, D);            // d*y^2
    gfSub(num, num, GF1);          // u = y^2 - 1
    gfAdd(den, GF1, den);          // v = d*y^2 + 1

    gfSquare(den2, den);
    gfSquare(den4, den2);
    gfMul(den6, den4, den2);
    gfMul(t, den6, num);
    gfMul(t, t, den);              // u * v^7

    gfPow2523(t, t);
    gfMul(t, t, num);
    gfMul(t, t, den);
    gfMul(t, t, den);
    gfMul(r[0], t, den);           // candidate x = u*v^3 * (u*v^7)^((p-5)/8)

    gfSquare(chk, r[0]);
    gfMul(chk, chk, den);
    if (!gfEqual(chk, num)) gfMul(r[0], r[0], SQRTM1);

    gfSquare(chk, r[0]);
    gfMul(chk, chk, den);
    if (!gfEqual(chk, num)) return false;   // y is not on the curve

    // Normally x is negated when its parity disagrees with the sign bit. The
    // test is inverted here so what comes out is -A rather than A.
    if (gfParity(r[0]) == (pk[31] >> 7)) gfSub(r[0], GF0, r[0]);

    gfMul(r[3], r[0], r[1]);
    return true;
}

// ── Scalar reduction mod L ────────────────────────────────────────────────

void modL(uint8_t r[32], int64_t x[64]) {
    int64_t carry;
    int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * ORDER_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * ORDER_L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; ++j) x[j] -= carry * ORDER_L[j];
    for (i = 0; i < 32; ++i) {
        r[i] = (uint8_t)(x[i] & 255);
        x[i + 1] += x[i] >> 8;
    }
}

// Reduces a 64-byte hash in place; only the first 32 bytes are meaningful
// afterwards.
void reduceModL(uint8_t r[64]) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (int64_t)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}

// True when the 32-byte little-endian value is strictly below the group
// order. Rejecting S >= L is what closes off signature malleability.
bool scalarBelowOrder(const uint8_t s[32]) {
    for (int i = 31; i >= 0; i--) {
        if ((int64_t)s[i] < ORDER_L[i]) return true;
        if ((int64_t)s[i] > ORDER_L[i]) return false;
    }
    return false;   // exactly L is not below L
}

// True when the y coordinate packed in the low 255 bits is a canonical field
// element. Two encodings of one key would defeat pinning by key id.
bool fieldBelowP(const uint8_t v[32]) {
    uint8_t y[32];
    memcpy(y, v, 32);
    y[31] &= 0x7f;
    for (int i = 31; i >= 0; i--) {
        if (y[i] < FIELD_P[i]) return true;
        if (y[i] > FIELD_P[i]) return false;
    }
    return false;
}

}  // namespace

namespace ContentCrypto {

void sha512Init(Sha512Ctx& ctx) {
#if CONTENT_PORTABLE_SHA512
    sha512InitPortable(ctx);
#else
    mbedtls_sha512_init(&ctx.md);
    // Second argument 0 selects SHA-512 rather than SHA-384.
    (void)HH_SHA512_STARTS(&ctx.md, 0);
#endif
}

void sha512Update(Sha512Ctx& ctx, const uint8_t* data, size_t len) {
    if (len == 0 || data == nullptr) return;
#if CONTENT_PORTABLE_SHA512
    sha512UpdatePortable(ctx, data, len);
#else
    (void)HH_SHA512_UPDATE(&ctx.md, data, len);
#endif
}

void sha512Finish(Sha512Ctx& ctx, uint8_t out[CONTENT_SHA512_BYTES]) {
    if (out == nullptr) return;
#if CONTENT_PORTABLE_SHA512
    sha512FinalPortable(ctx, out);
#else
    // Fail closed. A zeroed digest cannot match a real one, so a build where
    // SHA-512 is missing or the accelerator errors refuses every signature
    // instead of accepting every signature, and selfTest() says so at boot.
    if (HH_SHA512_FINISH(&ctx.md, out) != 0) {
        memset(out, 0, CONTENT_SHA512_BYTES);
    }
    mbedtls_sha512_free(&ctx.md);
#endif
}

void sha512n(const uint8_t* const* parts, const size_t* lens, size_t count,
             uint8_t out[CONTENT_SHA512_BYTES]) {
    if (out == nullptr) return;

    // Built ON the incremental context on purpose. One implementation of the
    // algorithm, so the one-shot form and the streaming form cannot disagree.
    Sha512Ctx ctx;
    sha512Init(ctx);
    for (size_t i = 0; i < count; i++) {
        if (lens[i] && parts[i]) sha512Update(ctx, parts[i], lens[i]);
    }
    sha512Finish(ctx, out);
}

void sha512(const uint8_t* a, size_t aLen,
            const uint8_t* b, size_t bLen,
            const uint8_t* c, size_t cLen,
            uint8_t out[CONTENT_SHA512_BYTES]) {
    const uint8_t* parts[3] = { a, b, c };
    const size_t   lens[3]  = { aLen, bLen, cLen };
    sha512n(parts, lens, 3, out);
}

bool equalCT(const uint8_t* a, const uint8_t* b, size_t len) {
    if (a == nullptr || b == nullptr) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff = (uint8_t)(diff | (uint8_t)(a[i] ^ b[i]));
    }
    return diff == 0;
}

bool verify(const uint8_t* msg, size_t msgLen,
            const uint8_t sig[ED25519_SIG_BYTES],
            const uint8_t pubkey[ED25519_PUBKEY_BYTES]) {
    return verify2(msg, msgLen, nullptr, 0, sig, pubkey);
}

bool verify2(const uint8_t* msg1, size_t msg1Len,
             const uint8_t* msg2, size_t msg2Len,
             const uint8_t sig[ED25519_SIG_BYTES],
             const uint8_t pubkey[ED25519_PUBKEY_BYTES]) {
    if (sig == nullptr || pubkey == nullptr) return false;
    if (msg1Len > 0 && msg1 == nullptr) return false;
    if (msg2Len > 0 && msg2 == nullptr) return false;

    // Cheap structural rejections first, so a malformed signature never
    // reaches the curve code at all.
    if (!scalarBelowOrder(sig + 32)) return false;
    if (!fieldBelowP(pubkey)) return false;

    gf negA[4];
    if (!edUnpackNeg(negA, pubkey)) return false;

    uint8_t h[CONTENT_SHA512_BYTES];
    {
        const uint8_t* parts[4] = { sig, pubkey, msg1, msg2 };
        const size_t   lens[4]  = { 32, 32, msg1Len, msg2Len };
        sha512n(parts, lens, 4, h);
    }
    reduceModL(h);

    gf p[4];
    edScalarMul(p, negA, h);       // -[h]A

    gf q[4];
    edScalarBase(q, sig + 32);     // [S]B

    edAdd(p, q);                   // [S]B - [h]A

    uint8_t computed[32];
    edPack(computed, p);
    return equalCT(sig, computed, 32);
}

bool selfTest() {
    // RFC 8032 section 7.1, TEST 2. A one-byte message, so a padding mistake
    // in SHA-512 shows up rather than cancelling, and short enough that the
    // vector fits here without hiding what it is.
    static const uint8_t pk[32] = {
        0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a,
        0x92, 0xb7, 0x0a, 0xa7, 0x4d, 0x1b, 0x7e, 0xbc,
        0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c,
        0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c
    };
    static const uint8_t msg[1] = { 0x72 };
    static const uint8_t sig[64] = {
        0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8,
        0x72, 0x0e, 0x82, 0x0b, 0x5f, 0x64, 0x25, 0x40,
        0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50, 0x3f, 0x8f,
        0xb3, 0x76, 0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda,
        0x08, 0x5a, 0xc1, 0xe4, 0x3e, 0x15, 0x99, 0x6e,
        0x45, 0x8f, 0x36, 0x13, 0xd0, 0xf1, 0x1d, 0x8c,
        0x38, 0x7b, 0x2e, 0xae, 0xb4, 0x30, 0x2a, 0xee,
        0xb0, 0x0d, 0x29, 0x16, 0x12, 0xbb, 0x0c, 0x00
    };

    if (!verify(msg, sizeof(msg), sig, pk)) return false;

    // Proving it accepts is only half of it. A verifier stuck at "true" is
    // worse than no verifier, because everything downstream would look fine.
    uint8_t bad[64];
    memcpy(bad, sig, 64);
    bad[10] ^= 0x01;
    if (verify(msg, sizeof(msg), bad, pk)) return false;

    static const uint8_t otherMsg[1] = { 0x73 };
    if (verify(otherMsg, sizeof(otherMsg), sig, pk)) return false;

    return true;
}

const char* backendName() {
#if CONTENT_PORTABLE_SHA512
    return "portable-sha512+ed25519";
#else
    return "mbedtls-sha512+ed25519";
#endif
}

}  // namespace ContentCrypto
