/*
 * RIPEMD-160 implementation
 * Nettle-compatible API (init/update/digest)
 * Based on the reference from the RIPEMD project (Dobbertin, Bosselaers, Preneel).
 * Public domain.
 */
#include "nettle_compat.h"
#include <string.h>

#define ROTL32(x,n) (((x) << (n)) | ((x) >> (32-(n))))

/* Boolean functions */
#define F0(x,y,z) ((x) ^ (y) ^ (z))
#define F1(x,y,z) (((x) & (y)) | (~(x) & (z)))
#define F2(x,y,z) (((x) | ~(y)) ^ (z))
#define F3(x,y,z) (((x) & (z)) | ((y) & ~(z)))
#define F4(x,y,z) ((x) ^ ((y) | ~(z)))

/* Round constants */
#define K0  0x00000000
#define K1  0x5A827999
#define K2  0x6ED9EBA1
#define K3  0x8F1BBCDC
#define K4  0xA953FD4E
#define KK0 0x50A28BE6
#define KK1 0x5C4DD124
#define KK2 0x6D703EF3
#define KK3 0x7A6D76E9
#define KK4 0x00000000

#define R(f,a,b,c,d,e,x,k,s) \
    (a) += f((b),(c),(d)) + (x) + (k); \
    (a) = ROTL32((a),(s)) + (e); \
    (c) = ROTL32((c),10);

static void ripemd160_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t X[16];
    uint32_t a, b, c, d, e;
    uint32_t aa, bb, cc, dd, ee;

    for (int i = 0; i < 16; i++)
        X[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) | ((uint32_t)block[i*4+3] << 24);

    a = aa = state[0];
    b = bb = state[1];
    c = cc = state[2];
    d = dd = state[3];
    e = ee = state[4];

    /* Left rounds */
    /* Round 1 */
    R(F0, a,b,c,d,e, X[0],  K0, 11) R(F0, e,a,b,c,d, X[1],  K0, 14)
    R(F0, d,e,a,b,c, X[2],  K0, 15) R(F0, c,d,e,a,b, X[3],  K0, 12)
    R(F0, b,c,d,e,a, X[4],  K0,  5) R(F0, a,b,c,d,e, X[5],  K0,  8)
    R(F0, e,a,b,c,d, X[6],  K0,  7) R(F0, d,e,a,b,c, X[7],  K0,  9)
    R(F0, c,d,e,a,b, X[8],  K0, 11) R(F0, b,c,d,e,a, X[9],  K0, 13)
    R(F0, a,b,c,d,e, X[10], K0, 14) R(F0, e,a,b,c,d, X[11], K0, 15)
    R(F0, d,e,a,b,c, X[12], K0,  6) R(F0, c,d,e,a,b, X[13], K0,  7)
    R(F0, b,c,d,e,a, X[14], K0,  9) R(F0, a,b,c,d,e, X[15], K0,  8)

    /* Round 2 */
    R(F1, e,a,b,c,d, X[7],  K1,  7) R(F1, d,e,a,b,c, X[4],  K1,  6)
    R(F1, c,d,e,a,b, X[13], K1,  8) R(F1, b,c,d,e,a, X[1],  K1, 13)
    R(F1, a,b,c,d,e, X[10], K1, 11) R(F1, e,a,b,c,d, X[6],  K1,  9)
    R(F1, d,e,a,b,c, X[15], K1,  7) R(F1, c,d,e,a,b, X[3],  K1, 15)
    R(F1, b,c,d,e,a, X[12], K1,  7) R(F1, a,b,c,d,e, X[0],  K1, 12)
    R(F1, e,a,b,c,d, X[9],  K1, 15) R(F1, d,e,a,b,c, X[5],  K1,  9)
    R(F1, c,d,e,a,b, X[2],  K1, 11) R(F1, b,c,d,e,a, X[14], K1,  7)
    R(F1, a,b,c,d,e, X[11], K1, 13) R(F1, e,a,b,c,d, X[8],  K1, 12)

    /* Round 3 */
    R(F2, d,e,a,b,c, X[3],  K2, 11) R(F2, c,d,e,a,b, X[10], K2, 13)
    R(F2, b,c,d,e,a, X[14], K2,  6) R(F2, a,b,c,d,e, X[4],  K2,  7)
    R(F2, e,a,b,c,d, X[9],  K2, 14) R(F2, d,e,a,b,c, X[15], K2,  9)
    R(F2, c,d,e,a,b, X[8],  K2, 13) R(F2, b,c,d,e,a, X[1],  K2, 15)
    R(F2, a,b,c,d,e, X[2],  K2, 14) R(F2, e,a,b,c,d, X[7],  K2,  8)
    R(F2, d,e,a,b,c, X[0],  K2, 13) R(F2, c,d,e,a,b, X[6],  K2,  6)
    R(F2, b,c,d,e,a, X[13], K2,  5) R(F2, a,b,c,d,e, X[11], K2, 12)
    R(F2, e,a,b,c,d, X[5],  K2,  7) R(F2, d,e,a,b,c, X[12], K2,  5)

    /* Round 4 */
    R(F3, c,d,e,a,b, X[1],  K3, 11) R(F3, b,c,d,e,a, X[9],  K3, 12)
    R(F3, a,b,c,d,e, X[11], K3, 14) R(F3, e,a,b,c,d, X[10], K3, 15)
    R(F3, d,e,a,b,c, X[0],  K3, 14) R(F3, c,d,e,a,b, X[8],  K3, 15)
    R(F3, b,c,d,e,a, X[12], K3,  9) R(F3, a,b,c,d,e, X[4],  K3,  8)
    R(F3, e,a,b,c,d, X[13], K3,  9) R(F3, d,e,a,b,c, X[3],  K3, 14)
    R(F3, c,d,e,a,b, X[7],  K3,  5) R(F3, b,c,d,e,a, X[15], K3,  6)
    R(F3, a,b,c,d,e, X[14], K3,  8) R(F3, e,a,b,c,d, X[5],  K3,  6)
    R(F3, d,e,a,b,c, X[6],  K3,  5) R(F3, c,d,e,a,b, X[2],  K3, 12)

    /* Round 5 */
    R(F4, b,c,d,e,a, X[4],  K4,  9) R(F4, a,b,c,d,e, X[0],  K4, 15)
    R(F4, e,a,b,c,d, X[5],  K4,  5) R(F4, d,e,a,b,c, X[9],  K4, 11)
    R(F4, c,d,e,a,b, X[7],  K4,  6) R(F4, b,c,d,e,a, X[12], K4,  8)
    R(F4, a,b,c,d,e, X[2],  K4, 13) R(F4, e,a,b,c,d, X[10], K4, 12)
    R(F4, d,e,a,b,c, X[14], K4,  5) R(F4, c,d,e,a,b, X[1],  K4, 12)
    R(F4, b,c,d,e,a, X[3],  K4, 13) R(F4, a,b,c,d,e, X[8],  K4, 14)
    R(F4, e,a,b,c,d, X[11], K4, 11) R(F4, d,e,a,b,c, X[6],  K4,  8)
    R(F4, c,d,e,a,b, X[15], K4,  5) R(F4, b,c,d,e,a, X[13], K4,  6)

    /* Right rounds (parallel) */
    /* Round 1' */
    R(F4, aa,bb,cc,dd,ee, X[5],  KK0,  8) R(F4, ee,aa,bb,cc,dd, X[14], KK0,  9)
    R(F4, dd,ee,aa,bb,cc, X[7],  KK0,  9) R(F4, cc,dd,ee,aa,bb, X[0],  KK0, 11)
    R(F4, bb,cc,dd,ee,aa, X[9],  KK0, 13) R(F4, aa,bb,cc,dd,ee, X[2],  KK0, 15)
    R(F4, ee,aa,bb,cc,dd, X[11], KK0, 15) R(F4, dd,ee,aa,bb,cc, X[4],  KK0,  5)
    R(F4, cc,dd,ee,aa,bb, X[13], KK0,  7) R(F4, bb,cc,dd,ee,aa, X[6],  KK0,  7)
    R(F4, aa,bb,cc,dd,ee, X[15], KK0,  8) R(F4, ee,aa,bb,cc,dd, X[8],  KK0, 11)
    R(F4, dd,ee,aa,bb,cc, X[1],  KK0, 14) R(F4, cc,dd,ee,aa,bb, X[10], KK0, 14)
    R(F4, bb,cc,dd,ee,aa, X[3],  KK0, 12) R(F4, aa,bb,cc,dd,ee, X[12], KK0,  6)

    /* Round 2' */
    R(F3, ee,aa,bb,cc,dd, X[6],  KK1,  9) R(F3, dd,ee,aa,bb,cc, X[11], KK1, 13)
    R(F3, cc,dd,ee,aa,bb, X[3],  KK1, 15) R(F3, bb,cc,dd,ee,aa, X[7],  KK1,  7)
    R(F3, aa,bb,cc,dd,ee, X[0],  KK1, 12) R(F3, ee,aa,bb,cc,dd, X[13], KK1,  8)
    R(F3, dd,ee,aa,bb,cc, X[5],  KK1,  9) R(F3, cc,dd,ee,aa,bb, X[10], KK1, 11)
    R(F3, bb,cc,dd,ee,aa, X[14], KK1,  7) R(F3, aa,bb,cc,dd,ee, X[15], KK1,  7)
    R(F3, ee,aa,bb,cc,dd, X[8],  KK1, 12) R(F3, dd,ee,aa,bb,cc, X[12], KK1,  7)
    R(F3, cc,dd,ee,aa,bb, X[4],  KK1,  6) R(F3, bb,cc,dd,ee,aa, X[9],  KK1, 15)
    R(F3, aa,bb,cc,dd,ee, X[1],  KK1, 13) R(F3, ee,aa,bb,cc,dd, X[2],  KK1, 11)

    /* Round 3' */
    R(F2, dd,ee,aa,bb,cc, X[15], KK2,  9) R(F2, cc,dd,ee,aa,bb, X[5],  KK2,  7)
    R(F2, bb,cc,dd,ee,aa, X[1],  KK2, 15) R(F2, aa,bb,cc,dd,ee, X[3],  KK2, 11)
    R(F2, ee,aa,bb,cc,dd, X[7],  KK2,  8) R(F2, dd,ee,aa,bb,cc, X[14], KK2,  6)
    R(F2, cc,dd,ee,aa,bb, X[6],  KK2,  6) R(F2, bb,cc,dd,ee,aa, X[9],  KK2, 14)
    R(F2, aa,bb,cc,dd,ee, X[11], KK2, 12) R(F2, ee,aa,bb,cc,dd, X[8],  KK2, 13)
    R(F2, dd,ee,aa,bb,cc, X[12], KK2,  5) R(F2, cc,dd,ee,aa,bb, X[2],  KK2, 14)
    R(F2, bb,cc,dd,ee,aa, X[10], KK2, 13) R(F2, aa,bb,cc,dd,ee, X[0],  KK2, 13)
    R(F2, ee,aa,bb,cc,dd, X[4],  KK2,  7) R(F2, dd,ee,aa,bb,cc, X[13], KK2,  5)

    /* Round 4' */
    R(F1, cc,dd,ee,aa,bb, X[8],  KK3, 15) R(F1, bb,cc,dd,ee,aa, X[6],  KK3,  5)
    R(F1, aa,bb,cc,dd,ee, X[4],  KK3,  8) R(F1, ee,aa,bb,cc,dd, X[1],  KK3, 11)
    R(F1, dd,ee,aa,bb,cc, X[3],  KK3, 14) R(F1, cc,dd,ee,aa,bb, X[11], KK3, 14)
    R(F1, bb,cc,dd,ee,aa, X[15], KK3,  6) R(F1, aa,bb,cc,dd,ee, X[0],  KK3, 14)
    R(F1, ee,aa,bb,cc,dd, X[5],  KK3,  6) R(F1, dd,ee,aa,bb,cc, X[12], KK3,  9)
    R(F1, cc,dd,ee,aa,bb, X[2],  KK3, 12) R(F1, bb,cc,dd,ee,aa, X[13], KK3,  9)
    R(F1, aa,bb,cc,dd,ee, X[9],  KK3, 12) R(F1, ee,aa,bb,cc,dd, X[7],  KK3,  5)
    R(F1, dd,ee,aa,bb,cc, X[10], KK3, 15) R(F1, cc,dd,ee,aa,bb, X[14], KK3,  8)

    /* Round 5' */
    R(F0, bb,cc,dd,ee,aa, X[12], KK4,  8) R(F0, aa,bb,cc,dd,ee, X[15], KK4,  5)
    R(F0, ee,aa,bb,cc,dd, X[10], KK4, 12) R(F0, dd,ee,aa,bb,cc, X[4],  KK4,  9)
    R(F0, cc,dd,ee,aa,bb, X[1],  KK4, 12) R(F0, bb,cc,dd,ee,aa, X[5],  KK4,  5)
    R(F0, aa,bb,cc,dd,ee, X[8],  KK4, 14) R(F0, ee,aa,bb,cc,dd, X[7],  KK4,  6)
    R(F0, dd,ee,aa,bb,cc, X[6],  KK4,  8) R(F0, cc,dd,ee,aa,bb, X[2],  KK4, 13)
    R(F0, bb,cc,dd,ee,aa, X[13], KK4,  6) R(F0, aa,bb,cc,dd,ee, X[14], KK4,  5)
    R(F0, ee,aa,bb,cc,dd, X[0],  KK4, 15) R(F0, dd,ee,aa,bb,cc, X[3],  KK4, 13)
    R(F0, cc,dd,ee,aa,bb, X[9],  KK4, 11) R(F0, bb,cc,dd,ee,aa, X[11], KK4, 11)

    /* Final addition */
    uint32_t t = state[1] + c + dd;
    state[1] = state[2] + d + ee;
    state[2] = state[3] + e + aa;
    state[3] = state[4] + a + bb;
    state[4] = state[0] + b + cc;
    state[0] = t;
}

void ripemd160_init(struct ripemd160_ctx *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

void ripemd160_update(struct ripemd160_ctx *ctx, unsigned length, const uint8_t *data)
{
    unsigned index = (unsigned)(ctx->count & 63);
    ctx->count += length;

    if (index) {
        unsigned part = 64 - index;
        if (length >= part) {
            memcpy(ctx->buffer + index, data, part);
            ripemd160_transform(ctx->state, ctx->buffer);
            data += part;
            length -= part;
        } else {
            memcpy(ctx->buffer + index, data, length);
            return;
        }
    }

    while (length >= 64) {
        ripemd160_transform(ctx->state, data);
        data += 64;
        length -= 64;
    }

    if (length)
        memcpy(ctx->buffer, data, length);
}

void ripemd160_digest(struct ripemd160_ctx *ctx, unsigned length, uint8_t *digest)
{
    uint64_t bits = ctx->count * 8;
    unsigned index = (unsigned)(ctx->count & 63);

    ctx->buffer[index++] = 0x80;
    if (index > 56) {
        memset(ctx->buffer + index, 0, 64 - index);
        ripemd160_transform(ctx->state, ctx->buffer);
        index = 0;
    }
    memset(ctx->buffer + index, 0, 56 - index);

    /* Append length in bits (little-endian) */
    for (int i = 0; i < 8; i++)
        ctx->buffer[56 + i] = (uint8_t)(bits >> (i * 8));

    ripemd160_transform(ctx->state, ctx->buffer);

    /* Output (little-endian) */
    unsigned out = length < 20 ? length : 20;
    for (unsigned i = 0; i < out; i++)
        digest[i] = (uint8_t)(ctx->state[i / 4] >> ((i % 4) * 8));

    ripemd160_init(ctx);
}
