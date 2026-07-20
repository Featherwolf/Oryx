// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryxmap.c — offline memory-class map: format, sound decision, reference
 * classifier, and liboryxcache integration. Fail-safe by construction.
 */
#define _GNU_SOURCE
#include "oryxmap.h"

#include <stdlib.h>
#include <string.h>

/* ---- little-endian ------------------------------------------------------- */
static void put_u16(uint8_t *p, uint16_t v){ p[0]=v; p[1]=v>>8; }
static void put_u32(uint8_t *p, uint32_t v){ for(int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void put_u64(uint8_t *p, uint64_t v){ for(int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint16_t get_u16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }
static uint32_t get_u32(const uint8_t *p){ uint32_t v=0; for(int i=0;i<4;i++) v|=(uint32_t)p[i]<<(8*i); return v; }
static uint64_t get_u64(const uint8_t *p){ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v; }

#define MMAP_HDR   56
#define MMAP_ENT   12

/* ---- serialize ----------------------------------------------------------- */
int oryx_mmap_serialize(const struct oryx_mmap *m, uint8_t **out, size_t *out_len)
{
	if (!m || !out || !out_len)
		return ORYX_ERR_INVAL;
	size_t total = MMAP_HDR + (size_t)m->entry_count * MMAP_ENT;
	uint8_t *b = malloc(total);
	if (!b)
		return ORYX_ERR_NOMEM;

	put_u32(b + 0,  ORYX_MMAP_MAGIC);
	put_u16(b + 4,  ORYX_MMAP_VERSION);
	put_u16(b + 6,  m->flags);
	put_u32(b + 8,  m->isa_id);
	put_u32(b + 12, m->analyzer_id);
	put_u32(b + 16, m->analyzer_version);
	put_u32(b + 20, m->entry_count);
	memcpy(b + 24, m->module_hash, SHA256_DIGEST_LEN);

	uint8_t *p = b + MMAP_HDR;
	for (uint32_t i = 0; i < m->entry_count; i++) {
		put_u64(p + 0, m->entries[i].guest_pc);
		p[8] = m->entries[i].mclass;
		p[9] = m->entries[i].eflags;
		put_u16(p + 10, 0);
		p += MMAP_ENT;
	}
	*out = b; *out_len = total;
	return ORYX_OK;
}

/* ---- parse (validates magic/version/exact length/sorted) ----------------- */
int oryx_mmap_parse(const uint8_t *buf, size_t len, struct oryx_mmap *out)
{
	if (!buf || !out || len < MMAP_HDR)
		return ORYX_ERR_FORMAT;
	if (get_u32(buf + 0) != ORYX_MMAP_MAGIC || get_u16(buf + 4) != ORYX_MMAP_VERSION)
		return ORYX_ERR_FORMAT;

	memset(out, 0, sizeof(*out));
	out->flags            = get_u16(buf + 6);
	out->isa_id           = get_u32(buf + 8);
	out->analyzer_id      = get_u32(buf + 12);
	out->analyzer_version = get_u32(buf + 16);
	out->entry_count      = get_u32(buf + 20);
	memcpy(out->module_hash, buf + 24, SHA256_DIGEST_LEN);

	size_t need = MMAP_HDR + (size_t)out->entry_count * MMAP_ENT;
	if (need != len)
		return ORYX_ERR_FORMAT;

	if (out->entry_count) {
		out->entries = calloc(out->entry_count, sizeof(*out->entries));
		if (!out->entries)
			return ORYX_ERR_NOMEM;
		const uint8_t *p = buf + MMAP_HDR;
		uint64_t prev = 0; int first = 1;
		for (uint32_t i = 0; i < out->entry_count; i++) {
			uint64_t pc = get_u64(p);
			/* must be strictly ascending for binary search + no ambiguity */
			if (!first && pc <= prev) { free(out->entries); out->entries = NULL; return ORYX_ERR_FORMAT; }
			out->entries[i].guest_pc = pc;
			out->entries[i].mclass   = p[8];
			out->entries[i].eflags   = p[9];
			prev = pc; first = 0;
			p += MMAP_ENT;
		}
	}
	return ORYX_OK;
}

void oryx_mmap_free(struct oryx_mmap *m)
{
	if (m) { free(m->entries); m->entries = NULL; m->entry_count = 0; }
}

/* ---- lookup -------------------------------------------------------------- */
const struct oryx_mmap_entry *oryx_mmap_lookup(const struct oryx_mmap *m, uint64_t guest_pc)
{
	if (!m || !m->entries)
		return NULL;
	uint32_t lo = 0, hi = m->entry_count;   /* [lo,hi) */
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		uint64_t pc = m->entries[mid].guest_pc;
		if (pc == guest_pc) return &m->entries[mid];
		if (pc < guest_pc)  lo = mid + 1;
		else                hi = mid;
	}
	return NULL;
}

/* ---- the sound decision function ---------------------------------------- */
enum oryx_order oryx_mmap_decide(const struct oryx_mmap_entry *e, int guard_confirmed)
{
	if (!e)
		return ORYX_ORD_TSO;              /* UNKNOWN -> order */
	switch (e->mclass) {
	case OMC_ATOMIC:
		return ORYX_ORD_ATOMIC;
	case OMC_LOCAL:
		if (e->eflags & OMC_EF_ESCAPED)
			return ORYX_ORD_TSO;     /* escape found -> treat as shared */
		if (!(e->eflags & OMC_EF_GUARD_REQUIRED))
			return ORYX_ORD_RELAX;   /* escape-proof automatic (guard-free) */
		return guard_confirmed ? ORYX_ORD_RELAX : ORYX_ORD_TSO;  /* fail safe */
	case OMC_SHARED:
	case OMC_UNKNOWN:
	default:
		return ORYX_ORD_TSO;
	}
}

/* ---- identity ------------------------------------------------------------ */
int oryx_mmap_verify_identity(const struct oryx_mmap *m,
			      const uint8_t module_hash[SHA256_DIGEST_LEN],
			      uint32_t analyzer_id, uint32_t analyzer_version, uint32_t isa_id)
{
	if (!m || !module_hash)
		return ORYX_ERR_INVAL;
	if (memcmp(m->module_hash, module_hash, SHA256_DIGEST_LEN) != 0)
		return ORYX_ERR_INTEGRITY;
	if (m->analyzer_id != analyzer_id ||
	    m->analyzer_version != analyzer_version || m->isa_id != isa_id)
		return ORYX_ERR_INTEGRITY;
	return ORYX_OK;
}

void oryx_mmap_logical_key(const struct oryx_mmap *m, char hex[SHA256_HEX_LEN + 1])
{
	uint8_t pre[SHA256_DIGEST_LEN + 12];
	memcpy(pre, m->module_hash, SHA256_DIGEST_LEN);
	put_u32(pre + 32, m->analyzer_id);
	put_u32(pre + 36, m->analyzer_version);
	put_u32(pre + 40, m->isa_id);
	uint8_t d[SHA256_DIGEST_LEN];
	sha256(pre, sizeof(pre), d);
	sha256_hex(d, hex);
}

/* ---- reference classifier ------------------------------------------------ */
int oryx_mmap_classify(const struct oryx_ginsn *ops, size_t n, uint64_t guest_pc,
		       const void *module_ident, size_t module_ident_len,
		       uint32_t isa_id, uint32_t analyzer_id, uint32_t analyzer_version,
		       struct oryx_mmap *out)
{
	if (!ops || !out)
		return ORYX_ERR_INVAL;
	memset(out, 0, sizeof(*out));
	out->isa_id = isa_id;
	out->analyzer_id = analyzer_id;
	out->analyzer_version = analyzer_version;
	/* Guard both pointer AND length: a NULL ident hashes the empty string, never
	 * a NULL+len OOB read. */
	sha256(module_ident ? module_ident : (const void *)"",
	       module_ident ? module_ident_len : 0, out->module_hash);

	struct oryx_mmap_entry *ents = calloc(n ? n : 1, sizeof(*ents));
	if (!ents)
		return ORYX_ERR_NOMEM;
	uint32_t k = 0;
	for (size_t i = 0; i < n; i++) {
		const struct oryx_ginsn *in = &ops[i];
		uint8_t cls, ef = 0;
		switch (in->op) {
		case GOP_LOAD:
		case GOP_STORE:
			/* Over-approximate: only stack (RSP/RBP) is LOCAL, and only as a
			 * guard-required hint (a stack address can escape). Everything else
			 * is SHARED. A real analyzer refines this with escape/alias info. */
			if (in->rn == GR_RSP || in->rn == GR_RBP) {
				cls = OMC_LOCAL; ef = OMC_EF_GUARD_REQUIRED;
			} else {
				cls = OMC_SHARED;
			}
			break;
		case GOP_ATOMIC_ADD:
		case GOP_ATOMIC_CAS:
			cls = OMC_ATOMIC;
			break;
		default:
			continue;   /* no memory access -> no entry */
		}
		ents[k].guest_pc = guest_pc + (uint64_t)i;  /* synthetic per-access id */
		ents[k].mclass = cls;
		ents[k].eflags = ef;
		k++;
	}
	out->entries = ents;
	out->entry_count = k;
	return ORYX_OK;
}

/* ---- liboryxcache integration ------------------------------------------- */
int oryx_mmap_cache_put(struct oryx_cache *c, const struct oryx_mmap *m,
			char addr_out[SHA256_HEX_LEN + 1])
{
	uint8_t *bytes; size_t len;
	int rc = oryx_mmap_serialize(m, &bytes, &len);
	if (rc != ORYX_OK)
		return rc;
	char key[SHA256_HEX_LEN + 1];
	oryx_mmap_logical_key(m, key);
	rc = oryx_cache_put_blob(c, bytes, len, key, addr_out);
	free(bytes);
	return rc;
}

int oryx_mmap_cache_get(struct oryx_cache *c,
			const uint8_t module_hash[SHA256_DIGEST_LEN],
			uint32_t analyzer_id, uint32_t analyzer_version, uint32_t isa_id,
			struct oryx_mmap *out)
{
	/* Reconstruct the logical key from the requested identity. */
	struct oryx_mmap probe;
	memset(&probe, 0, sizeof(probe));
	memcpy(probe.module_hash, module_hash, SHA256_DIGEST_LEN);
	probe.analyzer_id = analyzer_id;
	probe.analyzer_version = analyzer_version;
	probe.isa_id = isa_id;
	char key[SHA256_HEX_LEN + 1];
	oryx_mmap_logical_key(&probe, key);

	char addr[SHA256_HEX_LEN + 1];
	int rc = oryx_cache_resolve(c, key, addr);
	if (rc != ORYX_OK)
		return rc;
	uint8_t *bytes; size_t len;
	rc = oryx_cache_get_blob(c, addr, &bytes, &len);   /* verifies content addr */
	if (rc != ORYX_OK)
		return rc;
	rc = oryx_mmap_parse(bytes, len, out);
	free(bytes);
	if (rc != ORYX_OK)
		return rc;
	/* Fail closed if the stored map's identity doesn't match what we asked for. */
	rc = oryx_mmap_verify_identity(out, module_hash, analyzer_id, analyzer_version, isa_id);
	if (rc != ORYX_OK) {
		oryx_mmap_free(out);
		return rc;
	}
	return ORYX_OK;
}
