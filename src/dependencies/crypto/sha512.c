/*
 * SHA-512 and SHA-384 implementation -- FIPS 180-4
 * Nettle-compatible API (init/update/digest)
 * Public domain.
 */
#include "nettle_compat.h"
#include <string.h>

#define ROTR64(x,n) (((x) >> (n)) | ((x) << (64-(n))))
#define CH(x,y,z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)       (ROTR64(x,28) ^ ROTR64(x,34) ^ ROTR64(x,39))
#define EP1(x)       (ROTR64(x,14) ^ ROTR64(x,18) ^ ROTR64(x,41))
#define SIG0(x)      (ROTR64(x, 1) ^ ROTR64(x, 8) ^ ((x) >> 7))
#define SIG1(x)      (ROTR64(x,19) ^ ROTR64(x,61) ^ ((x) >> 6))

static const uint64_t K512[80] = {
    UINT64_C(0x428a2f98d728ae22), UINT64_C(0x7137449123ef65cd),
    UINT64_C(0xb5c0fbcfec4d3b2f), UINT64_C(0xe9b5dba58189dbbc),
    UINT64_C(0x3956c25bf348b538), UINT64_C(0x59f111f1b605d019),
    UINT64_C(0x923f82a4af194f9b), UINT64_C(0xab1c5ed5da6d8118),
    UINT64_C(0xd807aa98a3030242), UINT64_C(0x12835b0145706fbe),
    UINT64_C(0x243185be4ee4b28c), UINT64_C(0x550c7dc3d5ffb4e2),
    UINT64_C(0x72be5d74f27b896f), UINT64_C(0x80deb1fe3b1696b1),
    UINT64_C(0x9bdc06a725c71235), UINT64_C(0xc19bf174cf692694),
    UINT64_C(0xe49b69c19ef14ad2), UINT64_C(0xefbe4786384f25e3),
    UINT64_C(0x0fc19dc68b8cd5b5), UINT64_C(0x240ca1cc77ac9c65),
    UINT64_C(0x2de92c6f592b0275), UINT64_C(0x4a7484aa6ea6e483),
    UINT64_C(0x5cb0a9dcbd41fbd4), UINT64_C(0x76f988da831153b5),
    UINT64_C(0x983e5152ee66dfab), UINT64_C(0xa831c66d2db43210),
    UINT64_C(0xb00327c898fb213f), UINT64_C(0xbf597fc7beef0ee4),
    UINT64_C(0xc6e00bf33da88fc2), UINT64_C(0xd5a79147930aa725),
    UINT64_C(0x06ca6351e003826f), UINT64_C(0x142929670a0e6e70),
    UINT64_C(0x27b70a8546d22ffc), UINT64_C(0x2e1b21385c26c926),
    UINT64_C(0x4d2c6dfc5ac42aed), UINT64_C(0x53380d139d95b3df),
    UINT64_C(0x650a73548baf63de), UINT64_C(0x766a0abb3c77b2a8),
    UINT64_C(0x81c2c92e47edaee6), UINT64_C(0x92722c851482353b),
    UINT64_C(0xa2bfe8a14cf10364), UINT64_C(0xa81a664bbc423001),
    UINT64_C(0xc24b8b70d0f89791), UINT64_C(0xc76c51a30654be30),
    UINT64_C(0xd192e819d6ef5218), UINT64_C(0xd69906245565a910),
    UINT64_C(0xf40e35855771202a), UINT64_C(0x106aa07032bbd1b8),
    UINT64_C(0x19a4c116b8d2d0c8), UINT64_C(0x1e376c085141ab53),
    UINT64_C(0x2748774cdf8eeb99), UINT64_C(0x34b0bcb5e19b48a8),
    UINT64_C(0x391c0cb3c5c95a63), UINT64_C(0x4ed8aa4ae3418acb),
    UINT64_C(0x5b9cca4f7763e373), UINT64_C(0x682e6ff3d6b2b8a3),
    UINT64_C(0x748f82ee5defb2fc), UINT64_C(0x78a5636f43172f60),
    UINT64_C(0x84c87814a1f0ab72), UINT64_C(0x8cc702081a6439ec),
    UINT64_C(0x90befffa23631e28), UINT64_C(0xa4506cebde82bde9),
    UINT64_C(0xbef9a3f7b2c67915), UINT64_C(0xc67178f2e372532b),
    UINT64_C(0xca273eceea26619c), UINT64_C(0xd186b8c721c0c207),
    UINT64_C(0xeada7dd6cde0eb1e), UINT64_C(0xf57d4f7fee6ed178),
    UINT64_C(0x06f067aa72176fba), UINT64_C(0x0a637dc5a2c898a6),
    UINT64_C(0x113f9804bef90dae), UINT64_C(0x1b710b35131c471b),
    UINT64_C(0x28db77f523047d84), UINT64_C(0x32caab7b40c72493),
    UINT64_C(0x3c9ebe0a15c9bebc), UINT64_C(0x431d67c49c100d4c),
    UINT64_C(0x4cc5d4becb3e42b6), UINT64_C(0x597f299cfc657e2a),
    UINT64_C(0x5fcb6fab3ad6faec), UINT64_C(0x6c44198c4a475817)
};

static void sha512_transform(uint64_t state[8], const uint8_t block[128])
{
    uint64_t W[80];
    uint64_t a, b, c, d, e, f, g, h, t1, t2;

    for (int i = 0; i < 16; i++) {
        W[i] = ((uint64_t)block[i*8]   << 56) | ((uint64_t)block[i*8+1] << 48) |
               ((uint64_t)block[i*8+2] << 40) | ((uint64_t)block[i*8+3] << 32) |
               ((uint64_t)block[i*8+4] << 24) | ((uint64_t)block[i*8+5] << 16) |
               ((uint64_t)block[i*8+6] << 8)  | (uint64_t)block[i*8+7];
    }
    for (int i = 16; i < 80; i++)
        W[i] = SIG1(W[i-2]) + W[i-7] + SIG0(W[i-15]) + W[i-16];

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (int i = 0; i < 80; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + K512[i] + W[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha512_init(struct sha512_ctx *ctx)
{
    ctx->state[0] = UINT64_C(0x6a09e667f3bcc908);
    ctx->state[1] = UINT64_C(0xbb67ae8584caa73b);
    ctx->state[2] = UINT64_C(0x3c6ef372fe94f82b);
    ctx->state[3] = UINT64_C(0xa54ff53a5f1d36f1);
    ctx->state[4] = UINT64_C(0x510e527fade682d1);
    ctx->state[5] = UINT64_C(0x9b05688c2b3e6c1f);
    ctx->state[6] = UINT64_C(0x1f83d9abfb41bd6b);
    ctx->state[7] = UINT64_C(0x5be0cd19137e2179);
    ctx->count_low = ctx->count_high = 0;
    ctx->index = 0;
}

void sha512_update(struct sha512_ctx *ctx, unsigned length, const uint8_t *data)
{
    unsigned index = ctx->index;

    /* Update 128-bit count */
    uint64_t old_low = ctx->count_low;
    ctx->count_low += length;
    if (ctx->count_low < old_low)
        ctx->count_high++;

    if (index) {
        unsigned part = 128 - index;
        if (length >= part) {
            memcpy(ctx->buffer + index, data, part);
            sha512_transform(ctx->state, ctx->buffer);
            data += part;
            length -= part;
            index = 0;
        } else {
            memcpy(ctx->buffer + index, data, length);
            ctx->index = index + length;
            return;
        }
    }

    while (length >= 128) {
        sha512_transform(ctx->state, data);
        data += 128;
        length -= 128;
    }

    if (length) {
        memcpy(ctx->buffer, data, length);
    }
    ctx->index = length;
}

void sha512_digest(struct sha512_ctx *ctx, unsigned length, uint8_t *digest)
{
    uint64_t bits_low  = ctx->count_low * 8;
    uint64_t bits_high = ctx->count_high * 8 + (ctx->count_low >> 61);
    unsigned index = ctx->index;

    ctx->buffer[index++] = 0x80;
    if (index > 112) {
        memset(ctx->buffer + index, 0, 128 - index);
        sha512_transform(ctx->state, ctx->buffer);
        index = 0;
    }
    memset(ctx->buffer + index, 0, 112 - index);

    /* Append length in bits (big-endian, 128-bit) */
    for (int i = 0; i < 8; i++) {
        ctx->buffer[112 + i] = (uint8_t)(bits_high >> ((7 - i) * 8));
        ctx->buffer[120 + i] = (uint8_t)(bits_low  >> ((7 - i) * 8));
    }

    sha512_transform(ctx->state, ctx->buffer);

    unsigned out = length < 64 ? length : 64;
    for (unsigned i = 0; i < out; i++)
        digest[i] = (uint8_t)(ctx->state[i / 8] >> ((7 - (i % 8)) * 8));

    sha512_init(ctx);
}

/* SHA-384: same algorithm as SHA-512 but different IV and truncated output */
void sha384_init(struct sha512_ctx *ctx)
{
    ctx->state[0] = UINT64_C(0xcbbb9d5dc1059ed8);
    ctx->state[1] = UINT64_C(0x629a292a367cd507);
    ctx->state[2] = UINT64_C(0x9159015a3070dd17);
    ctx->state[3] = UINT64_C(0x152fecd8f70e5939);
    ctx->state[4] = UINT64_C(0x67332667ffc00b31);
    ctx->state[5] = UINT64_C(0x8eb44a8768581511);
    ctx->state[6] = UINT64_C(0xdb0c2e0d64f98fa7);
    ctx->state[7] = UINT64_C(0x47b5481dbefa4fa4);
    ctx->count_low = ctx->count_high = 0;
    ctx->index = 0;
}

void sha384_update(struct sha512_ctx *ctx, unsigned length, const uint8_t *data)
{
    sha512_update(ctx, length, data);
}

void sha384_digest(struct sha512_ctx *ctx, unsigned length, uint8_t *digest)
{
    uint64_t bits_low  = ctx->count_low * 8;
    uint64_t bits_high = ctx->count_high * 8 + (ctx->count_low >> 61);
    unsigned index = ctx->index;

    ctx->buffer[index++] = 0x80;
    if (index > 112) {
        memset(ctx->buffer + index, 0, 128 - index);
        sha512_transform(ctx->state, ctx->buffer);
        index = 0;
    }
    memset(ctx->buffer + index, 0, 112 - index);

    for (int i = 0; i < 8; i++) {
        ctx->buffer[112 + i] = (uint8_t)(bits_high >> ((7 - i) * 8));
        ctx->buffer[120 + i] = (uint8_t)(bits_low  >> ((7 - i) * 8));
    }

    sha512_transform(ctx->state, ctx->buffer);

    /* SHA-384 outputs only 48 bytes (6 words) */
    unsigned out = length < 48 ? length : 48;
    for (unsigned i = 0; i < out; i++)
        digest[i] = (uint8_t)(ctx->state[i / 8] >> ((7 - (i % 8)) * 8));

    sha384_init(ctx);
}
