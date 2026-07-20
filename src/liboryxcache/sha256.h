/* SPDX-License-Identifier: CC0-1.0
 * sha256.h — minimal, dependency-free SHA-256 (FIPS 180-4).
 */
#ifndef ORYX_SHA256_H
#define ORYX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32
#define SHA256_HEX_LEN    64   /* not counting NUL */

typedef struct {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t  buf[64];
	size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot helper. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

/* Format a digest as lowercase hex (writes SHA256_HEX_LEN+1 bytes incl. NUL). */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_LEN], char out[SHA256_HEX_LEN + 1]);

#endif /* ORYX_SHA256_H */
