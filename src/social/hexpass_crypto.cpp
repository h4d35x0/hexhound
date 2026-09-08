#include "hexpass_crypto.h"

#include <string.h>

#if !HEXPASS_PORTABLE_CRYPTO
#include <mbedtls/md.h>
#endif

// ── HexHound - HexPass Crypto Implementation ─────────────────────

namespace {

#if HEXPASS_PORTABLE_CRYPTO

// ── Portable SHA-256 (FIPS 180-4) ─────────────────────────────────────────
// Only compiled off-target. Deliberately the plain textbook construction: this
// code exists to be obviously correct against the published vectors, not to be
// fast. Nothing here allocates, so it is safe on the simulator and in tests.

const uint32_t K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

inline uint32_t ror32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

struct Sha256 {
    uint32_t h[8];
    uint64_t bitLen;
    uint8_t  buf[64];
    size_t   bufLen;
};

void shaCompress(Sha256& s, const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3];
    uint32_t e = s.h[4], f = s.h[5], g = s.h[6], hh = s.h[7];

    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        const uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d;
    s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += hh;
}

void shaInit(Sha256& s) {
    s.h[0] = 0x6a09e667UL; s.h[1] = 0xbb67ae85UL;
    s.h[2] = 0x3c6ef372UL; s.h[3] = 0xa54ff53aUL;
    s.h[4] = 0x510e527fUL; s.h[5] = 0x9b05688cUL;
    s.h[6] = 0x1f83d9abUL; s.h[7] = 0x5be0cd19UL;
    s.bitLen = 0;
    s.bufLen = 0;
}

void shaUpdate(Sha256& s, const uint8_t* data, size_t len) {
    s.bitLen += (uint64_t)len * 8u;
    while (len > 0) {
        const size_t take = (64 - s.bufLen < len) ? (64 - s.bufLen) : len;
        memcpy(s.buf + s.bufLen, data, take);
        s.bufLen += take;
        data     += take;
        len      -= take;
        if (s.bufLen == 64) {
            shaCompress(s, s.buf);
            s.bufLen = 0;
        }
    }
}

void shaFinal(Sha256& s, uint8_t out[32]) {
    const uint64_t bits = s.bitLen;
    const uint8_t  pad  = 0x80;
    shaUpdate(s, &pad, 1);
    // shaUpdate advanced bitLen; the length field must record the message
    // length, so it is captured above before padding starts.
    const uint8_t zero = 0x00;
    while (s.bufLen != 56) {
        shaUpdate(s, &zero, 1);
    }
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; i++) {
        lenBytes[i] = (uint8_t)(bits >> (56 - i * 8));
    }
    shaUpdate(s, lenBytes, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(s.h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s.h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s.h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s.h[i]);
    }
}

void sha256(const uint8_t* msg, size_t len, uint8_t out[32]) {
    Sha256 s;
    shaInit(s);
    if (len > 0 && msg != nullptr) {
        shaUpdate(s, msg, len);
    }
    shaFinal(s, out);
}

#endif  // HEXPASS_PORTABLE_CRYPTO

}  // namespace

namespace HexPassCrypto {

void hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* msg, size_t msgLen,
                uint8_t out[HEXPASS_SHA256_BYTES]) {
    if (out == nullptr) {
        return;
    }

#if HEXPASS_PORTABLE_CRYPTO
    // RFC 2104. Block size 64 for SHA-256.
    uint8_t k0[64];
    memset(k0, 0, sizeof(k0));
    if (keyLen > 64) {
        sha256(key, keyLen, k0);
    } else if (keyLen > 0 && key != nullptr) {
        memcpy(k0, key, keyLen);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36);
        opad[i] = (uint8_t)(k0[i] ^ 0x5c);
    }

    Sha256 inner;
    shaInit(inner);
    shaUpdate(inner, ipad, 64);
    if (msgLen > 0 && msg != nullptr) {
        shaUpdate(inner, msg, msgLen);
    }
    uint8_t innerDigest[32];
    shaFinal(inner, innerDigest);

    Sha256 outer;
    shaInit(outer);
    shaUpdate(outer, opad, 64);
    shaUpdate(outer, innerDigest, 32);
    shaFinal(outer, out);

    // Key material does not get to outlive its use on a device somebody else
    // may later hold. memset is enough here: these are stack buffers in a
    // function the compiler cannot prove is dead, and the alternative
    // (mbedtls_platform_zeroize) does not exist on this path.
    memset(k0, 0, sizeof(k0));
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
#else
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr) {
        // Cannot happen with SHA-256 compiled in, but returning zeros silently
        // would hand out a constant "identifier" for every device. Leave the
        // output zeroed and let hasSecret()/verify fail loudly instead.
        memset(out, 0, HEXPASS_SHA256_BYTES);
        return;
    }
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1) == 0 &&
        mbedtls_md_hmac_starts(&ctx, key, keyLen) == 0 &&
        mbedtls_md_hmac_update(&ctx, msg, msgLen) == 0 &&
        mbedtls_md_hmac_finish(&ctx, out) == 0) {
        // ok
    } else {
        memset(out, 0, HEXPASS_SHA256_BYTES);
    }
    mbedtls_md_free(&ctx);
#endif
}

bool equalCT(const uint8_t* a, const uint8_t* b, size_t len) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff = (uint8_t)(diff | (uint8_t)(a[i] ^ b[i]));
    }
    return diff == 0;
}

bool selfTest() {
    // RFC 4231 test case 2: key "Jefe", data "what do ya want for nothing?".
    // Chosen over case 1 because its key is shorter than the block size AND
    // not a repeated byte, so a padding bug shows up rather than cancelling.
    static const uint8_t key[]  = { 'J', 'e', 'f', 'e' };
    static const uint8_t data[] = {
        'w','h','a','t',' ','d','o',' ','y','a',' ','w','a','n','t',' ',
        'f','o','r',' ','n','o','t','h','i','n','g','?'
    };
    static const uint8_t expect[HEXPASS_SHA256_BYTES] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
        0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
        0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43
    };

    uint8_t got[HEXPASS_SHA256_BYTES];
    hmacSha256(key, sizeof(key), data, sizeof(data), got);
    return equalCT(got, expect, HEXPASS_SHA256_BYTES);
}

const char* backendName() {
#if HEXPASS_PORTABLE_CRYPTO
    return "portable";
#else
    return "mbedtls";
#endif
}

}  // namespace HexPassCrypto
