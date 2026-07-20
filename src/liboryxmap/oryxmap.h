/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * oryxmap.h — offline memory-class map: the no-root lever for Door 3.
 *
 * Heavy sharing/escape analysis runs OFF-device on a guest module and emits a
 * per-access memory class (UNKNOWN/LOCAL/SHARED/ATOMIC). The map is shipped via
 * liboryxcache keyed to the module's hash, and the on-device translator consults
 * it to pick each access's ordering. The whole point is soundness by
 * construction (docs/box64-fex-integration.md §3, docs/memory-class-map.md):
 *
 *   - Default is SHARED/ordered. Absence of an entry => UNKNOWN => ordered.
 *   - An offline LOCAL is only a HINT: it relaxes ordering ONLY when it either
 *     needs no runtime guard (statically proven) or a runtime guard confirms it.
 *     Otherwise it fails safe to ordered. Misclassifying SHARED as LOCAL would be
 *     a correctness bug, so the decision function never relaxes on doubt.
 *   - Identity is verified (module hash + analyzer version + isa) before use;
 *     a mismatch discards the map and the translator uses its sound static default.
 */
#ifndef ORYXMAP_H
#define ORYXMAP_H

#include <stddef.h>
#include <stdint.h>
#include "oryxcache.h"   /* error codes, sha256, cache store */
#include "oryxtu.h"      /* struct oryx_ginsn (reference classifier input) */

#ifdef __cplusplus
extern "C" {
#endif

#define ORYX_MMAP_MAGIC   0x31434d4f  /* "OMC1" little-endian */
#define ORYX_MMAP_VERSION 1

/* Memory class of an access. UNKNOWN and SHARED both order; the distinction is
 * provenance (UNKNOWN = no info; SHARED = analyzed and found possibly shared). */
enum oryx_mclass4 {
	OMC_UNKNOWN = 0,   /* no information -> must order (safe default)          */
	OMC_LOCAL   = 1,   /* proven/likely thread-local -> may relax (see flags)  */
	OMC_SHARED  = 2,   /* possibly shared -> must order                        */
	OMC_ATOMIC  = 3,   /* synchronization -> emit the atomic, never relax      */
};

/* Per-entry flags. */
#define OMC_EF_GUARD_REQUIRED (1u << 0) /* LOCAL relaxes only if a runtime guard confirms it */
#define OMC_EF_ESCAPED        (1u << 1) /* was LOCAL but downgraded to SHARED by escape analysis */

/* The ordering the translator must emit for an access. */
enum oryx_order {
	ORYX_ORD_RELAX  = 0,  /* plain weak load/store (LOCAL, confirmed)   */
	ORYX_ORD_TSO    = 1,  /* TSO-preserving ordered access (LDAR/DMB..) */
	ORYX_ORD_ATOMIC = 2,  /* LSE atomic                                  */
};

struct oryx_mmap_entry {
	uint64_t guest_pc;   /* address of the access this entry describes */
	uint8_t  mclass;     /* enum oryx_mclass4 */
	uint8_t  eflags;     /* OMC_EF_* */
};

struct oryx_mmap {
	uint32_t isa_id;
	uint32_t analyzer_id;
	uint32_t analyzer_version;
	uint16_t flags;
	uint8_t  module_hash[SHA256_DIGEST_LEN]; /* hash of the analyzed guest code */
	struct oryx_mmap_entry *entries;         /* sorted ascending by guest_pc */
	uint32_t entry_count;
};

/* ---- format ------------------------------------------------------------- */
int  oryx_mmap_serialize(const struct oryx_mmap *m, uint8_t **out, size_t *out_len);
int  oryx_mmap_parse(const uint8_t *buf, size_t len, struct oryx_mmap *out);
void oryx_mmap_free(struct oryx_mmap *m);

/* ---- lookup + the SOUND decision function ------------------------------- */
/* Binary search; returns NULL if no entry describes guest_pc (=> UNKNOWN). */
const struct oryx_mmap_entry *oryx_mmap_lookup(const struct oryx_mmap *m, uint64_t guest_pc);

/*
 * The one place the soundness contract lives. `e` may be NULL (no entry).
 * `guard_confirmed` is whether a runtime ownership/MTE guard has confirmed the
 * access is currently thread-local.
 *   ATOMIC                          -> ORYX_ORD_ATOMIC
 *   LOCAL, no guard required        -> ORYX_ORD_RELAX
 *   LOCAL, guard required + confirmed-> ORYX_ORD_RELAX
 *   LOCAL, guard required + NOT confirmed -> ORYX_ORD_TSO   (fail safe)
 *   SHARED / UNKNOWN / NULL / anything else -> ORYX_ORD_TSO
 */
enum oryx_order oryx_mmap_decide(const struct oryx_mmap_entry *e, int guard_confirmed);

/* ---- identity verification (fail closed on mismatch) -------------------- */
int oryx_mmap_verify_identity(const struct oryx_mmap *m,
			      const uint8_t module_hash[SHA256_DIGEST_LEN],
			      uint32_t analyzer_version, uint32_t isa_id);

/* Logical key for cache storage = hash(module_hash ‖ analyzer_id ‖ version ‖ isa). */
void oryx_mmap_logical_key(const struct oryx_mmap *m, char hex[SHA256_HEX_LEN + 1]);

/* ---- reference classifier (over the liboryxtu guest IR) ----------------- */
/*
 * Build a memory-class map for one basic block. Over-approximates SHARED: only
 * RSP/RBP-relative (stack) accesses become LOCAL (with GUARD_REQUIRED, since a
 * stack address can escape); atomics become ATOMIC; every other access is
 * SHARED. Entries are keyed by guest_pc + op index. module_hash is computed from
 * a caller-supplied module-identity buffer. This is the on-ramp for a real
 * off-device escape/alias analysis, not a substitute for one.
 */
int oryx_mmap_classify(const struct oryx_ginsn *ops, size_t n, uint64_t guest_pc,
		       const void *module_ident, size_t module_ident_len,
		       uint32_t isa_id, uint32_t analyzer_id, uint32_t analyzer_version,
		       struct oryx_mmap *out);

/* ---- liboryxcache integration ------------------------------------------- */
int oryx_mmap_cache_put(struct oryx_cache *c, const struct oryx_mmap *m,
			char addr_out[SHA256_HEX_LEN + 1]);
int oryx_mmap_cache_get(struct oryx_cache *c,
			const uint8_t module_hash[SHA256_DIGEST_LEN],
			uint32_t analyzer_id, uint32_t analyzer_version, uint32_t isa_id,
			struct oryx_mmap *out);

#ifdef __cplusplus
}
#endif

#endif /* ORYXMAP_H */
