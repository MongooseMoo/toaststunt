/*
 * SHA-1 implementation -- FIPS 180-4
 * Nettle-compatible API (init/update/digest)
 * Public domain.
 */
#include "nettle_compat.h"
#include <string.h>

#define ROTL32(x,n) (((x) << (n)) | ((x) >> (32-(n))))

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t W[80];
    uint32_t a, b, c, d, e, temp;

    /* Prepare message schedule (big-endian) */
    for (int i = 0; i < 16; i++)
        W[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    for (int i = 16; i < 80; i++)
        W[i] = ROTL32(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1);

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;                    k = 0xCA62C1D6; }

        temp = ROTL32(a, 5) + f + e + k + W[i];
        e = d; d = c; c = ROTL32(b, 30); b = a; a = temp;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void sha1_init(struct sha1_ctx *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

void sha1_update(struct sha1_ctx *ctx, unsigned length, const uint8_t *data)
{
    unsigned index = (unsigned)(ctx->count & 63);
    ctx->count += length;

    if (index) {
        unsigned part = 64 - index;
        if (length >= part) {
            memcpy(ctx->buffer + index, data, part);
            sha1_transform(ctx->state, ctx->buffer);
            data += part;
            length -= part;
        } else {
            memcpy(ctx->buffer + index, data, length);
            return;
        }
    }

    while (length >= 64) {
        sha1_transform(ctx->state, data);
        data += 64;
        length -= 64;
    }

    if (length)
        memcpy(ctx->buffer, data, length);
}

void sha1_digest(struct sha1_ctx *ctx, unsigned length, uint8_t *digest)
{
    uint64_t bits = ctx->count * 8;
    unsigned index = (unsigned)(ctx->count & 63);

    ctx->buffer[index++] = 0x80;
    if (index > 56) {
        memset(ctx->buffer + index, 0, 64 - index);
        sha1_transform(ctx->state, ctx->buffer);
        index = 0;
    }
    memset(ctx->buffer + index, 0, 56 - index);

    /* Append length in bits (big-endian) */
    for (int i = 0; i < 8; i++)
        ctx->buffer[56 + i] = (uint8_t)(bits >> ((7 - i) * 8));

    sha1_transform(ctx->state, ctx->buffer);

    /* Output (big-endian) */
    unsigned out = length < 20 ? length : 20;
    for (unsigned i = 0; i < out; i++)
        digest[i] = (uint8_t)(ctx->state[i / 4] >> ((3 - (i % 4)) * 8));

    sha1_init(ctx);
}
