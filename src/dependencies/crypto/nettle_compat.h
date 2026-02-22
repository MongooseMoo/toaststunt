/*
 * nettle_compat.h -- Nettle-compatible API for standalone hash implementations
 *
 * Provides the same context types and function signatures that crypto.cc
 * expects from <nettle/md5.h>, <nettle/sha1.h>, <nettle/sha2.h>,
 * <nettle/ripemd160.h>, and <nettle/hmac.h>.
 *
 * All implementations are standalone C with no external dependencies.
 * Public domain / CC0.
 */
#ifndef NETTLE_COMPAT_H
#define NETTLE_COMPAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- MD5 ---- */
struct md5_ctx {
    uint32_t state[4];
    uint64_t count;
    uint8_t buffer[64];
};

void md5_init(struct md5_ctx *ctx);
void md5_update(struct md5_ctx *ctx, unsigned length, const uint8_t *data);
void md5_digest(struct md5_ctx *ctx, unsigned length, uint8_t *digest);

/* ---- SHA-1 ---- */
struct sha1_ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
};

void sha1_init(struct sha1_ctx *ctx);
void sha1_update(struct sha1_ctx *ctx, unsigned length, const uint8_t *data);
void sha1_digest(struct sha1_ctx *ctx, unsigned length, uint8_t *digest);

/* ---- SHA-256 / SHA-224 ---- */
struct sha256_ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
};

/* SHA-224 shares the same context structure as SHA-256 */
typedef struct sha256_ctx sha224_ctx;

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, unsigned length, const uint8_t *data);
void sha256_digest(struct sha256_ctx *ctx, unsigned length, uint8_t *digest);

void sha224_init(struct sha256_ctx *ctx);
void sha224_update(struct sha256_ctx *ctx, unsigned length, const uint8_t *data);
void sha224_digest(struct sha256_ctx *ctx, unsigned length, uint8_t *digest);

/* ---- SHA-512 / SHA-384 ---- */
struct sha512_ctx {
    uint64_t state[8];
    uint64_t count_low, count_high;
    uint8_t buffer[128];
    unsigned index;
};

/* SHA-384 shares the same context structure as SHA-512 */
typedef struct sha512_ctx sha384_ctx;

void sha512_init(struct sha512_ctx *ctx);
void sha512_update(struct sha512_ctx *ctx, unsigned length, const uint8_t *data);
void sha512_digest(struct sha512_ctx *ctx, unsigned length, uint8_t *digest);

void sha384_init(struct sha512_ctx *ctx);
void sha384_update(struct sha512_ctx *ctx, unsigned length, const uint8_t *data);
void sha384_digest(struct sha512_ctx *ctx, unsigned length, uint8_t *digest);

/* ---- RIPEMD-160 ---- */
struct ripemd160_ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
};

void ripemd160_init(struct ripemd160_ctx *ctx);
void ripemd160_update(struct ripemd160_ctx *ctx, unsigned length, const uint8_t *data);
void ripemd160_digest(struct ripemd160_ctx *ctx, unsigned length, uint8_t *digest);

/* ---- HMAC ---- */

/* HMAC-SHA1 */
struct hmac_sha1_ctx {
    struct sha1_ctx outer;
    struct sha1_ctx inner;
    struct sha1_ctx state;
};

void hmac_sha1_set_key(struct hmac_sha1_ctx *ctx, unsigned key_length, const uint8_t *key);
void hmac_sha1_update(struct hmac_sha1_ctx *ctx, unsigned length, const uint8_t *data);
void hmac_sha1_digest(struct hmac_sha1_ctx *ctx, unsigned length, uint8_t *digest);

/* HMAC-SHA256 */
struct hmac_sha256_ctx {
    struct sha256_ctx outer;
    struct sha256_ctx inner;
    struct sha256_ctx state;
};

void hmac_sha256_set_key(struct hmac_sha256_ctx *ctx, unsigned key_length, const uint8_t *key);
void hmac_sha256_update(struct hmac_sha256_ctx *ctx, unsigned length, const uint8_t *data);
void hmac_sha256_digest(struct hmac_sha256_ctx *ctx, unsigned length, uint8_t *digest);

#ifdef __cplusplus
}
#endif

#endif /* NETTLE_COMPAT_H */
