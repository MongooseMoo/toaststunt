/*
 * SHA-256 and SHA-224 implementation -- FIPS 180-4
 * Nettle-compatible API (init/update/digest)
 * Public domain.
 */
#include "nettle_compat.h"
#include <string.h>

#define ROTR32(x,n) (((x) >> (n)) | ((x) << (32-(n))))
#define CH(x,y,z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)       (ROTR32(x, 2) ^ ROTR32(x,13) ^ ROTR32(x,22))
#define EP1(x)       (ROTR32(x, 6) ^ ROTR32(x,11) ^ ROTR32(x,25))
#define SIG0(x)      (ROTR32(x, 7) ^ ROTR32(x,18) ^ ((x) >> 3))
#define SIG1(x)      (ROTR32(x,17) ^ ROTR32(x,19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;

    for (int i = 0; i < 16; i++)
        W[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    for (int i = 16; i < 64; i++)
        W[i] = SIG1(W[i-2]) + W[i-7] + SIG0(W[i-15]) + W[i-16];

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + K256[i] + W[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(struct sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void sha256_update(struct sha256_ctx *ctx, unsigned length, const uint8_t *data)
{
    unsigned index = (unsigned)(ctx->count & 63);
    ctx->count += length;

    if (index) {
        unsigned part = 64 - index;
        if (length >= part) {
            memcpy(ctx->buffer + index, data, part);
            sha256_transform(ctx->state, ctx->buffer);
            data += part;
            length -= part;
        } else {
            memcpy(ctx->buffer + index, data, length);
            return;
        }
    }

    while (length >= 64) {
        sha256_transform(ctx->state, data);
        data += 64;
        length -= 64;
    }

    if (length)
        memcpy(ctx->buffer, data, length);
}

void sha256_digest(struct sha256_ctx *ctx, unsigned length, uint8_t *digest)
{
    uint64_t bits = ctx->count * 8;
    unsigned index = (unsigned)(ctx->count & 63);

    ctx->buffer[index++] = 0x80;
    if (index > 56) {
        memset(ctx->buffer + index, 0, 64 - index);
        sha256_transform(ctx->state, ctx->buffer);
        index = 0;
    }
    memset(ctx->buffer + index, 0, 56 - index);

    /* Append length in bits (big-endian) */
    for (int i = 0; i < 8; i++)
        ctx->buffer[56 + i] = (uint8_t)(bits >> ((7 - i) * 8));

    sha256_transform(ctx->state, ctx->buffer);

    unsigned out = length < 32 ? length : 32;
    for (unsigned i = 0; i < out; i++)
        digest[i] = (uint8_t)(ctx->state[i / 4] >> ((3 - (i % 4)) * 8));

    sha256_init(ctx);
}

/* SHA-224: same algorithm as SHA-256 but different IV and truncated output */
void sha224_init(struct sha256_ctx *ctx)
{
    ctx->state[0] = 0xc1059ed8;
    ctx->state[1] = 0x367cd507;
    ctx->state[2] = 0x3070dd17;
    ctx->state[3] = 0xf70e5939;
    ctx->state[4] = 0xffc00b31;
    ctx->state[5] = 0x68581511;
    ctx->state[6] = 0x64f98fa7;
    ctx->state[7] = 0xbefa4fa4;
    ctx->count = 0;
}

void sha224_update(struct sha256_ctx *ctx, unsigned length, const uint8_t *data)
{
    sha256_update(ctx, length, data);
}

void sha224_digest(struct sha256_ctx *ctx, unsigned length, uint8_t *digest)
{
    uint64_t bits = ctx->count * 8;
    unsigned index = (unsigned)(ctx->count & 63);

    ctx->buffer[index++] = 0x80;
    if (index > 56) {
        memset(ctx->buffer + index, 0, 64 - index);
        sha256_transform(ctx->state, ctx->buffer);
        index = 0;
    }
    memset(ctx->buffer + index, 0, 56 - index);

    for (int i = 0; i < 8; i++)
        ctx->buffer[56 + i] = (uint8_t)(bits >> ((7 - i) * 8));

    sha256_transform(ctx->state, ctx->buffer);

    /* SHA-224 outputs only 28 bytes (7 words) */
    unsigned out = length < 28 ? length : 28;
    for (unsigned i = 0; i < out; i++)
        digest[i] = (uint8_t)(ctx->state[i / 4] >> ((3 - (i % 4)) * 8));

    sha224_init(ctx);
}
