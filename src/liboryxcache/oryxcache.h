/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * oryxcache.h — Project Oryx Part B: portable, content-addressed cache for
 * translated code and compiled shaders.
 *
 * Model (see docs/adr/0002 and docs/formats/translation-unit.md):
 *   - Every entry is stored as a BLOB named by its content address = SHA-256 of
 *     its canonical bytes. Retrieval re-hashes and rejects any mismatch, so a
 *     corrupted or tampered entry can never be executed. Fail-closed always.
 *   - A logical KEY (what a client looks up) maps to a content address via a
 *     small index. The key encodes (guest identity + translator/profile/isa) so
 *     an entry warmed on one S26 Ultra is served to every ISA-identical device.
 *   - Translation units are a typed blob (serialized TU); shaders are opaque
 *     blobs. Both share the same store, keyed differently.
 */
#ifndef ORYXCACHE_H
#define ORYXCACHE_H

#include <stddef.h>
#include <stdint.h>
#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Errors -------------------------------------------------------------- */
enum {
	ORYX_OK          =  0,
	ORYX_ERR_IO      = -1,
	ORYX_ERR_FORMAT  = -2,   /* bad magic/version/lengths */
	ORYX_ERR_INTEGRITY = -3, /* content address mismatch — refuse to use */
	ORYX_ERR_NOTFOUND = -4,
	ORYX_ERR_INVAL   = -5,
	ORYX_ERR_NOMEM   = -6,
};

/* ---- TranslationUnit format --------------------------------------------- */
#define ORYX_TU_MAGIC   0x3155544f  /* "OTU1" little-endian */
#define ORYX_TU_VERSION 1

/* Relocation kinds: how to patch a host-code site to a guest target at map time. */
enum {
	ORYX_RELOC_NONE            = 0,
	ORYX_RELOC_ABS64_GUEST_PC  = 1, /* store the resolved host addr of a guest PC */
	ORYX_RELOC_BRANCH_GUEST_PC = 2, /* patch a branch to the host addr of a guest PC */
};

struct oryx_reloc {
	uint32_t offset;        /* byte offset into code[] */
	uint32_t kind;          /* ORYX_RELOC_* */
	uint64_t guest_target;  /* guest PC this site refers to */
};

struct oryx_tu {
	uint32_t isa_id;             /* SM8850-class ISA identity guard */
	uint32_t translator_id;      /* e.g. box64 / fex */
	uint32_t translator_version;
	uint32_t profile_id;         /* tuning profile assumed by codegen (incl. mem-model) */
	uint32_t flags;
	uint64_t guest_entry_pc;
	uint32_t guest_len;          /* bytes of guest code covered */
	uint8_t  guest_hash[SHA256_DIGEST_LEN]; /* hash of the guest bytes */

	uint8_t          *code;      /* position-independent AArch64 */
	uint32_t          code_len;
	struct oryx_reloc *relocs;
	uint32_t          reloc_count;
	uint64_t         *exits;     /* successor guest PCs (for prefetch) */
	uint32_t          exit_count;
};

/* Serialize a TU to canonical little-endian bytes. Caller free()s *out. */
int oryx_tu_serialize(const struct oryx_tu *tu, uint8_t **out, size_t *out_len);

/* Parse canonical bytes into a TU (allocates code/relocs/exits; free with
 * oryx_tu_free). Validates magic, version, and that all lengths fit in buf. */
int oryx_tu_parse(const uint8_t *buf, size_t len, struct oryx_tu *out);

void oryx_tu_free(struct oryx_tu *tu);

/* Logical key = SHA-256(guest_hash ‖ translator_id ‖ translator_version ‖
 * profile_id ‖ isa_id). Two devices with the same inputs compute the same key. */
void oryx_tu_logical_key(const struct oryx_tu *tu, char hex[SHA256_HEX_LEN + 1]);

/* Content address of arbitrary bytes = SHA-256 hex. */
void oryx_content_address(const void *buf, size_t len, char hex[SHA256_HEX_LEN + 1]);

/* ---- Cache store --------------------------------------------------------- */
struct oryx_cache {
	char root[512];
};

/* Open (creating root/, root/blobs/, root/index/ if needed). */
int oryx_cache_open(struct oryx_cache *c, const char *root);

/* Store a blob under a logical key. Writes the blob atomically (named by its
 * content address) and points the index entry at it. Returns the content
 * address in addr_out. Idempotent for identical bytes. */
int oryx_cache_put_blob(struct oryx_cache *c,
			const void *data, size_t len,
			const char *logical_key_hex,
			char addr_out[SHA256_HEX_LEN + 1]);

/* Load a blob by content address, verifying integrity (re-hash == address).
 * Caller free()s *out. Returns ORYX_ERR_INTEGRITY on mismatch. */
int oryx_cache_get_blob(struct oryx_cache *c,
			const char *addr_hex,
			uint8_t **out, size_t *out_len);

/* Resolve a logical key to its content address via the index. */
int oryx_cache_resolve(struct oryx_cache *c,
		       const char *logical_key_hex,
		       char addr_out[SHA256_HEX_LEN + 1]);

/* Convenience: put a TU (serialize + put_blob under its logical key). */
int oryx_cache_put_tu(struct oryx_cache *c, const struct oryx_tu *tu,
		      char addr_out[SHA256_HEX_LEN + 1]);

/* Convenience: fetch + verify + parse a TU by its logical key. */
int oryx_cache_get_tu(struct oryx_cache *c, const char *logical_key_hex,
		      struct oryx_tu *out);

/* Verify integrity of a stored blob by content address (no parse). */
int oryx_cache_verify(struct oryx_cache *c, const char *addr_hex);

#ifdef __cplusplus
}
#endif

#endif /* ORYXCACHE_H */
