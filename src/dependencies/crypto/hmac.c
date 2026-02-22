/*
 * HMAC implementation -- RFC 2104
 * Nettle-compatible API for HMAC-SHA1 and HMAC-SHA256
 *
 * HMAC(K, m) = H((K' ^ opad) || H((K' ^ ipad) || m))
 * where ipad = 0x36 repeated, opad = 0x5c repeated
 *
 * Public domain.
 */
#include "nettle_compat.h"
#include <string.h>

/* ---- HMAC-SHA1 ---- */

#define SHA1_BLOCK_SIZE  64
#define SHA1_DIGEST_SIZE 20

void hmac_sha1_set_key(struct hmac_sha1_ctx *ctx, unsigned key_length, const uint8_t *key)
{
    uint8_t key_block[SHA1_BLOCK_SIZE];

    /* If key is longer than block size, hash it first */
    if (key_length > SHA1_BLOCK_SIZE) {
        struct sha1_ctx hctx;
        sha1_init(&hctx);
        sha1_update(&hctx, key_length, key);
        sha1_digest(&hctx, SHA1_DIGEST_SIZE, key_block);
        memset(key_block + SHA1_DIGEST_SIZE, 0, SHA1_BLOCK_SIZE - SHA1_DIGEST_SIZE);
    } else {
        memcpy(key_block, key, key_length);
        if (key_length < SHA1_BLOCK_SIZE)
            memset(key_block + key_length, 0, SHA1_BLOCK_SIZE - key_length);
    }

    /* Compute inner and outer padded keys, store the initialized contexts */
    uint8_t ipad[SHA1_BLOCK_SIZE];
    uint8_t opad[SHA1_BLOCK_SIZE];

    for (int i = 0; i < SHA1_BLOCK_SIZE; i++) {
        ipad[i] = key_block[i] ^ 0x36;
        opad[i] = key_block[i] ^ 0x5c;
    }

    /* inner context: H(ipad || ...) */
    sha1_init(&ctx->inner);
    sha1_update(&ctx->inner, SHA1_BLOCK_SIZE, ipad);

    /* outer context: H(opad || ...) */
    sha1_init(&ctx->outer);
    sha1_update(&ctx->outer, SHA1_BLOCK_SIZE, opad);

    /* Save the initial inner state for reuse */
    memcpy(&ctx->state, &ctx->inner, sizeof(struct sha1_ctx));
}

void hmac_sha1_update(struct hmac_sha1_ctx *ctx, unsigned length, const uint8_t *data)
{
    sha1_update(&ctx->state, length, data);
}

void hmac_sha1_digest(struct hmac_sha1_ctx *ctx, unsigned length, uint8_t *digest)
{
    uint8_t inner_digest[SHA1_DIGEST_SIZE];

    /* Finalize inner hash */
    sha1_digest(&ctx->state, SHA1_DIGEST_SIZE, inner_digest);

    /* Compute outer hash: H(opad || inner_digest) */
    struct sha1_ctx outer_copy;
    memcpy(&outer_copy, &ctx->outer, sizeof(struct sha1_ctx));
    sha1_update(&outer_copy, SHA1_DIGEST_SIZE, inner_digest);
    sha1_digest(&outer_copy, length, digest);

    /* Reset state for reuse (nettle behavior) */
    memcpy(&ctx->state, &ctx->inner, sizeof(struct sha1_ctx));
}

/* ---- HMAC-SHA256 ---- */

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

void hmac_sha256_set_key(struct hmac_sha256_ctx *ctx, unsigned key_length, const uint8_t *key)
{
    uint8_t key_block[SHA256_BLOCK_SIZE];

    if (key_length > SHA256_BLOCK_SIZE) {
        struct sha256_ctx hctx;
        sha256_init(&hctx);
        sha256_update(&hctx, key_length, key);
        sha256_digest(&hctx, SHA256_DIGEST_SIZE, key_block);
        memset(key_block + SHA256_DIGEST_SIZE, 0, SHA256_BLOCK_SIZE - SHA256_DIGEST_SIZE);
    } else {
        memcpy(key_block, key, key_length);
        if (key_length < SHA256_BLOCK_SIZE)
            memset(key_block + key_length, 0, SHA256_BLOCK_SIZE - key_length);
    }

    uint8_t ipad[SHA256_BLOCK_SIZE];
    uint8_t opad[SHA256_BLOCK_SIZE];

    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = key_block[i] ^ 0x36;
        opad[i] = key_block[i] ^ 0x5c;
    }

    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, SHA256_BLOCK_SIZE, ipad);

    sha256_init(&ctx->outer);
    sha256_update(&ctx->outer, SHA256_BLOCK_SIZE, opad);

    memcpy(&ctx->state, &ctx->inner, sizeof(struct sha256_ctx));
}

void hmac_sha256_update(struct hmac_sha256_ctx *ctx, unsigned length, const uint8_t *data)
{
    sha256_update(&ctx->state, length, data);
}

void hmac_sha256_digest(struct hmac_sha256_ctx *ctx, unsigned length, uint8_t *digest)
{
    uint8_t inner_digest[SHA256_DIGEST_SIZE];

    sha256_digest(&ctx->state, SHA256_DIGEST_SIZE, inner_digest);

    struct sha256_ctx outer_copy;
    memcpy(&outer_copy, &ctx->outer, sizeof(struct sha256_ctx));
    sha256_update(&outer_copy, SHA256_DIGEST_SIZE, inner_digest);
    sha256_digest(&outer_copy, length, digest);

    memcpy(&ctx->state, &ctx->inner, sizeof(struct sha256_ctx));
}
