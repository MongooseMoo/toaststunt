/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011 The FreeBSD Project. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Adapted for ToastStunt/Windows by using nettle library for SHA primitives.
 * Based on SHA256/SHA512-based Unix crypt implementation.
 * Released into the Public Domain by Ulrich Drepper <drepper@redhat.com>.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nettle/sha2.h>

#ifdef _WIN32
#include <windows.h>
#define secure_zero(ptr, len) SecureZeroMemory(ptr, len)
#else
#include <strings.h>
#define secure_zero(ptr, len) explicit_bzero(ptr, len)
#endif

/* MIN/MAX macros if not defined */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* Base64 encoding table for crypt */
static const char itoa64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void
b64_from_24bit(uint8_t B2, uint8_t B1, uint8_t B0, int n, char **cp)
{
    uint32_t w;
    int i;

    w = ((uint32_t)B2 << 16) | ((uint32_t)B1 << 8) | (uint32_t)B0;
    for (i = 0; i < n; i++) {
        **cp = itoa64[w & 0x3f];
        (*cp)++;
        w >>= 6;
    }
}

/* ========================================================================== */
/* SHA512 crypt ($6$)                                                         */
/* ========================================================================== */

static const char sha512_salt_prefix[] = "$6$";
static const char sha512_rounds_prefix[] = "rounds=";

#define SHA512_SALT_LEN_MAX 16
#define SHA512_ROUNDS_DEFAULT 5000
#define SHA512_ROUNDS_MIN 1000
#define SHA512_ROUNDS_MAX 999999999

char *
_crypt_sha512_rn(const char *key, const char *salt, char *buffer, int buflen)
{
    unsigned long srounds;
    uint8_t alt_result[64], temp_result[64];
    struct sha512_ctx ctx, alt_ctx;
    size_t salt_len, key_len, cnt, rounds;
    char *cp, *p_bytes, *s_bytes, *endp;
    const char *num;
    bool rounds_custom;

    /* Default number of rounds. */
    rounds = SHA512_ROUNDS_DEFAULT;
    rounds_custom = false;

    /* Find beginning of salt string. The prefix should normally always
     * be present. Just in case it is not. */
    if (strncmp(sha512_salt_prefix, salt, sizeof(sha512_salt_prefix) - 1) == 0)
        salt += sizeof(sha512_salt_prefix) - 1;

    if (strncmp(salt, sha512_rounds_prefix, sizeof(sha512_rounds_prefix) - 1) == 0) {
        num = salt + sizeof(sha512_rounds_prefix) - 1;
        srounds = strtoul(num, &endp, 10);

        if (*endp == '$') {
            salt = endp + 1;
            rounds = MAX(SHA512_ROUNDS_MIN, MIN(srounds, SHA512_ROUNDS_MAX));
            rounds_custom = true;
        }
    }

    salt_len = MIN(strcspn(salt, "$"), SHA512_SALT_LEN_MAX);
    key_len = strlen(key);

    /* Allocate p_bytes and s_bytes */
    p_bytes = (char *)malloc(key_len);
    s_bytes = (char *)malloc(salt_len);
    if (!p_bytes || !s_bytes) {
        free(p_bytes);
        free(s_bytes);
        return NULL;
    }

    /* Prepare for the real work. */
    sha512_init(&ctx);

    /* Add the key string. */
    sha512_update(&ctx, key_len, (const uint8_t *)key);

    /* The last part is the salt string. */
    sha512_update(&ctx, salt_len, (const uint8_t *)salt);

    /* Compute alternate SHA512 sum with input KEY, SALT, and KEY. */
    sha512_init(&alt_ctx);
    sha512_update(&alt_ctx, key_len, (const uint8_t *)key);
    sha512_update(&alt_ctx, salt_len, (const uint8_t *)salt);
    sha512_update(&alt_ctx, key_len, (const uint8_t *)key);
    sha512_digest(&alt_ctx, 64, alt_result);

    /* Add for any character in the key one byte of the alternate sum. */
    for (cnt = key_len; cnt > 64; cnt -= 64)
        sha512_update(&ctx, 64, alt_result);
    sha512_update(&ctx, cnt, alt_result);

    /* Take the binary representation of the length of the key and for
     * every 1 add the alternate sum, for every 0 the key. */
    for (cnt = key_len; cnt > 0; cnt >>= 1)
        if ((cnt & 1) != 0)
            sha512_update(&ctx, 64, alt_result);
        else
            sha512_update(&ctx, key_len, (const uint8_t *)key);

    /* Create intermediate result. */
    sha512_digest(&ctx, 64, alt_result);

    /* Start computation of P byte sequence. */
    sha512_init(&alt_ctx);
    for (cnt = 0; cnt < key_len; ++cnt)
        sha512_update(&alt_ctx, key_len, (const uint8_t *)key);
    sha512_digest(&alt_ctx, 64, temp_result);

    /* Create byte sequence P. */
    cp = p_bytes;
    for (cnt = key_len; cnt >= 64; cnt -= 64) {
        memcpy(cp, temp_result, 64);
        cp += 64;
    }
    memcpy(cp, temp_result, cnt);

    /* Start computation of S byte sequence. */
    sha512_init(&alt_ctx);
    for (cnt = 0; cnt < 16 + alt_result[0]; ++cnt)
        sha512_update(&alt_ctx, salt_len, (const uint8_t *)salt);
    sha512_digest(&alt_ctx, 64, temp_result);

    /* Create byte sequence S. */
    cp = s_bytes;
    for (cnt = salt_len; cnt >= 64; cnt -= 64) {
        memcpy(cp, temp_result, 64);
        cp += 64;
    }
    memcpy(cp, temp_result, cnt);

    /* Repeatedly run the collected hash value through SHA512 to burn CPU cycles. */
    for (cnt = 0; cnt < rounds; ++cnt) {
        sha512_init(&ctx);

        if ((cnt & 1) != 0)
            sha512_update(&ctx, key_len, (const uint8_t *)p_bytes);
        else
            sha512_update(&ctx, 64, alt_result);

        if (cnt % 3 != 0)
            sha512_update(&ctx, salt_len, (const uint8_t *)s_bytes);

        if (cnt % 7 != 0)
            sha512_update(&ctx, key_len, (const uint8_t *)p_bytes);

        if ((cnt & 1) != 0)
            sha512_update(&ctx, 64, alt_result);
        else
            sha512_update(&ctx, key_len, (const uint8_t *)p_bytes);

        sha512_digest(&ctx, 64, alt_result);
    }

    /* Now we can construct the result string. */
    cp = buffer;
    strcpy(cp, sha512_salt_prefix);
    cp += strlen(sha512_salt_prefix);

    if (rounds_custom) {
        cp += sprintf(cp, "%s%zu$", sha512_rounds_prefix, rounds);
    }

    strncpy(cp, salt, salt_len);
    cp += salt_len;
    *cp++ = '$';

    b64_from_24bit(alt_result[0], alt_result[21], alt_result[42], 4, &cp);
    b64_from_24bit(alt_result[22], alt_result[43], alt_result[1], 4, &cp);
    b64_from_24bit(alt_result[44], alt_result[2], alt_result[23], 4, &cp);
    b64_from_24bit(alt_result[3], alt_result[24], alt_result[45], 4, &cp);
    b64_from_24bit(alt_result[25], alt_result[46], alt_result[4], 4, &cp);
    b64_from_24bit(alt_result[47], alt_result[5], alt_result[26], 4, &cp);
    b64_from_24bit(alt_result[6], alt_result[27], alt_result[48], 4, &cp);
    b64_from_24bit(alt_result[28], alt_result[49], alt_result[7], 4, &cp);
    b64_from_24bit(alt_result[50], alt_result[8], alt_result[29], 4, &cp);
    b64_from_24bit(alt_result[9], alt_result[30], alt_result[51], 4, &cp);
    b64_from_24bit(alt_result[31], alt_result[52], alt_result[10], 4, &cp);
    b64_from_24bit(alt_result[53], alt_result[11], alt_result[32], 4, &cp);
    b64_from_24bit(alt_result[12], alt_result[33], alt_result[54], 4, &cp);
    b64_from_24bit(alt_result[34], alt_result[55], alt_result[13], 4, &cp);
    b64_from_24bit(alt_result[56], alt_result[14], alt_result[35], 4, &cp);
    b64_from_24bit(alt_result[15], alt_result[36], alt_result[57], 4, &cp);
    b64_from_24bit(alt_result[37], alt_result[58], alt_result[16], 4, &cp);
    b64_from_24bit(alt_result[59], alt_result[17], alt_result[38], 4, &cp);
    b64_from_24bit(alt_result[18], alt_result[39], alt_result[60], 4, &cp);
    b64_from_24bit(alt_result[40], alt_result[61], alt_result[19], 4, &cp);
    b64_from_24bit(alt_result[62], alt_result[20], alt_result[41], 4, &cp);
    b64_from_24bit(0, 0, alt_result[63], 2, &cp);

    *cp = '\0';

    /* Clear sensitive data */
    secure_zero(alt_result, sizeof(alt_result));
    secure_zero(temp_result, sizeof(temp_result));
    secure_zero(p_bytes, key_len);
    secure_zero(s_bytes, salt_len);
    free(p_bytes);
    free(s_bytes);

    return buffer;
}

/* ========================================================================== */
/* SHA256 crypt ($5$)                                                         */
/* ========================================================================== */

static const char sha256_salt_prefix[] = "$5$";
static const char sha256_rounds_prefix[] = "rounds=";

#define SHA256_SALT_LEN_MAX 16
#define SHA256_ROUNDS_DEFAULT 5000
#define SHA256_ROUNDS_MIN 1000
#define SHA256_ROUNDS_MAX 999999999

char *
_crypt_sha256_rn(const char *key, const char *salt, char *buffer, int buflen)
{
    unsigned long srounds;
    uint8_t alt_result[32], temp_result[32];
    struct sha256_ctx ctx, alt_ctx;
    size_t salt_len, key_len, cnt, rounds;
    char *cp, *p_bytes, *s_bytes, *endp;
    const char *num;
    bool rounds_custom;

    /* Default number of rounds. */
    rounds = SHA256_ROUNDS_DEFAULT;
    rounds_custom = false;

    /* Find beginning of salt string. */
    if (strncmp(sha256_salt_prefix, salt, sizeof(sha256_salt_prefix) - 1) == 0)
        salt += sizeof(sha256_salt_prefix) - 1;

    if (strncmp(salt, sha256_rounds_prefix, sizeof(sha256_rounds_prefix) - 1) == 0) {
        num = salt + sizeof(sha256_rounds_prefix) - 1;
        srounds = strtoul(num, &endp, 10);

        if (*endp == '$') {
            salt = endp + 1;
            rounds = MAX(SHA256_ROUNDS_MIN, MIN(srounds, SHA256_ROUNDS_MAX));
            rounds_custom = true;
        }
    }

    salt_len = MIN(strcspn(salt, "$"), SHA256_SALT_LEN_MAX);
    key_len = strlen(key);

    /* Allocate p_bytes and s_bytes */
    p_bytes = (char *)malloc(key_len);
    s_bytes = (char *)malloc(salt_len);
    if (!p_bytes || !s_bytes) {
        free(p_bytes);
        free(s_bytes);
        return NULL;
    }

    /* Prepare for the real work. */
    sha256_init(&ctx);
    sha256_update(&ctx, key_len, (const uint8_t *)key);
    sha256_update(&ctx, salt_len, (const uint8_t *)salt);

    /* Compute alternate SHA256 sum with input KEY, SALT, and KEY. */
    sha256_init(&alt_ctx);
    sha256_update(&alt_ctx, key_len, (const uint8_t *)key);
    sha256_update(&alt_ctx, salt_len, (const uint8_t *)salt);
    sha256_update(&alt_ctx, key_len, (const uint8_t *)key);
    sha256_digest(&alt_ctx, 32, alt_result);

    /* Add for any character in the key one byte of the alternate sum. */
    for (cnt = key_len; cnt > 32; cnt -= 32)
        sha256_update(&ctx, 32, alt_result);
    sha256_update(&ctx, cnt, alt_result);

    /* Take the binary representation of the length of the key and for
     * every 1 add the alternate sum, for every 0 the key. */
    for (cnt = key_len; cnt > 0; cnt >>= 1)
        if ((cnt & 1) != 0)
            sha256_update(&ctx, 32, alt_result);
        else
            sha256_update(&ctx, key_len, (const uint8_t *)key);

    /* Create intermediate result. */
    sha256_digest(&ctx, 32, alt_result);

    /* Start computation of P byte sequence. */
    sha256_init(&alt_ctx);
    for (cnt = 0; cnt < key_len; ++cnt)
        sha256_update(&alt_ctx, key_len, (const uint8_t *)key);
    sha256_digest(&alt_ctx, 32, temp_result);

    /* Create byte sequence P. */
    cp = p_bytes;
    for (cnt = key_len; cnt >= 32; cnt -= 32) {
        memcpy(cp, temp_result, 32);
        cp += 32;
    }
    memcpy(cp, temp_result, cnt);

    /* Start computation of S byte sequence. */
    sha256_init(&alt_ctx);
    for (cnt = 0; cnt < 16 + alt_result[0]; ++cnt)
        sha256_update(&alt_ctx, salt_len, (const uint8_t *)salt);
    sha256_digest(&alt_ctx, 32, temp_result);

    /* Create byte sequence S. */
    cp = s_bytes;
    for (cnt = salt_len; cnt >= 32; cnt -= 32) {
        memcpy(cp, temp_result, 32);
        cp += 32;
    }
    memcpy(cp, temp_result, cnt);

    /* Repeatedly run the collected hash value through SHA256 to burn CPU cycles. */
    for (cnt = 0; cnt < rounds; ++cnt) {
        sha256_init(&ctx);

        if ((cnt & 1) != 0)
            sha256_update(&ctx, key_len, (const uint8_t *)p_bytes);
        else
            sha256_update(&ctx, 32, alt_result);

        if (cnt % 3 != 0)
            sha256_update(&ctx, salt_len, (const uint8_t *)s_bytes);

        if (cnt % 7 != 0)
            sha256_update(&ctx, key_len, (const uint8_t *)p_bytes);

        if ((cnt & 1) != 0)
            sha256_update(&ctx, 32, alt_result);
        else
            sha256_update(&ctx, key_len, (const uint8_t *)p_bytes);

        sha256_digest(&ctx, 32, alt_result);
    }

    /* Now we can construct the result string. */
    cp = buffer;
    strcpy(cp, sha256_salt_prefix);
    cp += strlen(sha256_salt_prefix);

    if (rounds_custom) {
        cp += sprintf(cp, "%s%zu$", sha256_rounds_prefix, rounds);
    }

    strncpy(cp, salt, salt_len);
    cp += salt_len;
    *cp++ = '$';

    b64_from_24bit(alt_result[0], alt_result[10], alt_result[20], 4, &cp);
    b64_from_24bit(alt_result[21], alt_result[1], alt_result[11], 4, &cp);
    b64_from_24bit(alt_result[12], alt_result[22], alt_result[2], 4, &cp);
    b64_from_24bit(alt_result[3], alt_result[13], alt_result[23], 4, &cp);
    b64_from_24bit(alt_result[24], alt_result[4], alt_result[14], 4, &cp);
    b64_from_24bit(alt_result[15], alt_result[25], alt_result[5], 4, &cp);
    b64_from_24bit(alt_result[6], alt_result[16], alt_result[26], 4, &cp);
    b64_from_24bit(alt_result[27], alt_result[7], alt_result[17], 4, &cp);
    b64_from_24bit(alt_result[18], alt_result[28], alt_result[8], 4, &cp);
    b64_from_24bit(alt_result[9], alt_result[19], alt_result[29], 4, &cp);
    b64_from_24bit(0, alt_result[31], alt_result[30], 3, &cp);

    *cp = '\0';

    /* Clear sensitive data */
    secure_zero(alt_result, sizeof(alt_result));
    secure_zero(temp_result, sizeof(temp_result));
    secure_zero(p_bytes, key_len);
    secure_zero(s_bytes, salt_len);
    free(p_bytes);
    free(s_bytes);

    return buffer;
}
