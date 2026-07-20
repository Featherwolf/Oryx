// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * test_oryxmap.c — unit tests for the offline memory-class map. Heaviest focus
 * on the SOUND decision function (a wrong relaxation is silent memory
 * corruption) and fail-closed identity/integrity.
 */
#define _GNU_SOURCE
#include "oryxmap.h"
#include "oryxcache.h"
#include "oryxtu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); } } while (0)
#define TESTDIR "./_mapcache"
static void rm_rf(const char *d){ char c[600]; snprintf(c,sizeof(c),"rm -rf '%s'",d); if(system(c)){} }

static struct oryx_mmap_entry E(uint64_t pc, uint8_t cls, uint8_t ef)
{
	struct oryx_mmap_entry e = { .guest_pc = pc, .mclass = cls, .eflags = ef };
	return e;
}

static void test_decide_contract(void)
{
	printf("test: sound decision contract (the correctness-critical piece)\n");
	/* NULL entry (UNKNOWN) must order. */
	CHECK(oryx_mmap_decide(NULL, 0) == ORYX_ORD_TSO, "no entry -> TSO");
	CHECK(oryx_mmap_decide(NULL, 1) == ORYX_ORD_TSO, "no entry -> TSO even if 'confirmed'");

	struct oryx_mmap_entry atomic  = E(0, OMC_ATOMIC, 0);
	struct oryx_mmap_entry shared  = E(0, OMC_SHARED, 0);
	struct oryx_mmap_entry unknown = E(0, OMC_UNKNOWN, 0);
	struct oryx_mmap_entry local_free  = E(0, OMC_LOCAL, 0);                     /* statically proven */
	struct oryx_mmap_entry local_guard = E(0, OMC_LOCAL, OMC_EF_GUARD_REQUIRED); /* hint */

	CHECK(oryx_mmap_decide(&atomic, 0) == ORYX_ORD_ATOMIC, "ATOMIC -> ATOMIC");
	CHECK(oryx_mmap_decide(&shared, 1) == ORYX_ORD_TSO, "SHARED -> TSO (even if 'confirmed')");
	CHECK(oryx_mmap_decide(&unknown, 1) == ORYX_ORD_TSO, "UNKNOWN -> TSO");
	CHECK(oryx_mmap_decide(&local_free, 0) == ORYX_ORD_RELAX, "LOCAL no-guard -> RELAX");
	/* The fail-safe: a guard-required LOCAL that is NOT confirmed must ORDER. */
	CHECK(oryx_mmap_decide(&local_guard, 0) == ORYX_ORD_TSO, "LOCAL guard-required + unconfirmed -> TSO (FAIL SAFE)");
	CHECK(oryx_mmap_decide(&local_guard, 1) == ORYX_ORD_RELAX, "LOCAL guard-required + confirmed -> RELAX");
}

static void build_map(struct oryx_mmap *m)
{
	memset(m, 0, sizeof(*m));
	m->isa_id = ORYX_TU_ISA_SM8850;
	m->analyzer_id = 0x414e4c31; /* "ANL1" */
	m->analyzer_version = 7;
	sha256("guest-module-bytes", 18, m->module_hash);
	static struct oryx_mmap_entry ents[3];
	ents[0] = E(0x1000, OMC_LOCAL,  OMC_EF_GUARD_REQUIRED);
	ents[1] = E(0x1004, OMC_SHARED, 0);
	ents[2] = E(0x1008, OMC_ATOMIC, 0);
	m->entries = ents;
	m->entry_count = 3;
}

static void test_format_and_lookup(void)
{
	printf("test: format round-trip + lookup\n");
	struct oryx_mmap m; build_map(&m);
	uint8_t *b; size_t l;
	CHECK(oryx_mmap_serialize(&m, &b, &l) == ORYX_OK, "serialize");

	struct oryx_mmap got;
	CHECK(oryx_mmap_parse(b, l, &got) == ORYX_OK, "parse");
	CHECK(got.entry_count == 3 && got.analyzer_version == 7, "header preserved");
	CHECK(memcmp(got.module_hash, m.module_hash, 32) == 0, "module hash preserved");

	const struct oryx_mmap_entry *e = oryx_mmap_lookup(&got, 0x1004);
	CHECK(e && e->mclass == OMC_SHARED, "lookup 0x1004 -> SHARED");
	e = oryx_mmap_lookup(&got, 0x1000);
	CHECK(e && e->mclass == OMC_LOCAL && (e->eflags & OMC_EF_GUARD_REQUIRED), "lookup 0x1000 -> LOCAL+guard");
	CHECK(oryx_mmap_lookup(&got, 0x2000) == NULL, "lookup miss -> NULL (=> UNKNOWN => TSO)");

	oryx_mmap_free(&got);
	free(b);
}

static void test_parse_rejects(void)
{
	printf("test: parse rejects malformed / unsorted\n");
	struct oryx_mmap m; build_map(&m);
	uint8_t *b; size_t l;
	oryx_mmap_serialize(&m, &b, &l);
	struct oryx_mmap got;

	uint8_t save = b[0]; b[0] ^= 0xff;
	CHECK(oryx_mmap_parse(b, l, &got) == ORYX_ERR_FORMAT, "reject bad magic");
	b[0] = save;
	CHECK(oryx_mmap_parse(b, l - 1, &got) == ORYX_ERR_FORMAT, "reject wrong length");

	/* Corrupt entry ordering: make entry[1].guest_pc <= entry[0].guest_pc. */
	uint8_t *p1 = b + 56 + 12; /* second entry's guest_pc */
	memset(p1, 0, 8);          /* 0 <= first(0x1000) -> not strictly ascending */
	CHECK(oryx_mmap_parse(b, l, &got) == ORYX_ERR_FORMAT, "reject unsorted entries");
	free(b);
}

static void test_classifier(void)
{
	printf("test: reference classifier over guest IR\n");
	struct oryx_ginsn ops[6]; size_t n = 0;
	ops[n++] = (struct oryx_ginsn){ .op = GOP_STORE, .rd = GR_RAX, .rn = GR_RSP, .imm = 8 };   /* stack -> LOCAL */
	ops[n++] = (struct oryx_ginsn){ .op = GOP_MOV_RR, .rd = GR_RCX, .rn = GR_RDX };            /* no mem -> skip */
	ops[n++] = (struct oryx_ginsn){ .op = GOP_LOAD,  .rd = GR_RCX, .rn = GR_RDI, .imm = 0 };   /* general -> SHARED */
	ops[n++] = (struct oryx_ginsn){ .op = GOP_LOAD,  .rd = GR_RDX, .rn = GR_RBP, .imm = 16 };  /* frame -> LOCAL */
	ops[n++] = (struct oryx_ginsn){ .op = GOP_ATOMIC_ADD, .rd = GR_RCX, .rn = GR_RDI };        /* -> ATOMIC */
	ops[n++] = (struct oryx_ginsn){ .op = GOP_RET };

	struct oryx_mmap m;
	CHECK(oryx_mmap_classify(ops, n, 0x400000, "mod", 3, ORYX_TU_ISA_SM8850, 1, 1, &m) == ORYX_OK, "classify");
	CHECK(m.entry_count == 4, "4 memory accesses -> 4 entries (MOV and RET skipped)");

	const struct oryx_mmap_entry *e;
	e = oryx_mmap_lookup(&m, 0x400000 + 0); CHECK(e && e->mclass == OMC_LOCAL && (e->eflags & OMC_EF_GUARD_REQUIRED), "RSP store -> LOCAL+guard");
	e = oryx_mmap_lookup(&m, 0x400000 + 2); CHECK(e && e->mclass == OMC_SHARED, "general load -> SHARED");
	e = oryx_mmap_lookup(&m, 0x400000 + 3); CHECK(e && e->mclass == OMC_LOCAL, "RBP load -> LOCAL");
	e = oryx_mmap_lookup(&m, 0x400000 + 4); CHECK(e && e->mclass == OMC_ATOMIC, "atomic -> ATOMIC");

	/* End-to-end: classifier -> decide -> ordering the translator would emit. */
	e = oryx_mmap_lookup(&m, 0x400000 + 2);
	CHECK(oryx_mmap_decide(e, 1) == ORYX_ORD_TSO, "shared access always ordered");
	e = oryx_mmap_lookup(&m, 0x400000 + 0);
	CHECK(oryx_mmap_decide(e, 0) == ORYX_ORD_TSO, "stack access unconfirmed -> ordered (safe)");
	CHECK(oryx_mmap_decide(e, 1) == ORYX_ORD_RELAX, "stack access confirmed -> relaxed (the win)");
	oryx_mmap_free(&m);
}

static void test_identity_and_cache(void)
{
	printf("test: identity verification + cache round-trip + fail-closed\n");
	rm_rf(TESTDIR);
	struct oryx_cache c;
	CHECK(oryx_cache_open(&c, TESTDIR) == ORYX_OK, "cache open");

	struct oryx_mmap m; build_map(&m);
	char addr[65];
	CHECK(oryx_mmap_cache_put(&c, &m, addr) == ORYX_OK, "cache put map");

	struct oryx_mmap got;
	CHECK(oryx_mmap_cache_get(&c, m.module_hash, m.analyzer_id, m.analyzer_version, m.isa_id, &got) == ORYX_OK,
	      "cache get by identity");
	CHECK(got.entry_count == 3, "retrieved entries");
	oryx_mmap_free(&got);

	/* Identity checks. */
	uint8_t wrong[32]; memcpy(wrong, m.module_hash, 32); wrong[0] ^= 1;
	CHECK(oryx_mmap_verify_identity(&m, m.module_hash, m.analyzer_id, m.analyzer_version, m.isa_id) == ORYX_OK, "identity match");
	CHECK(oryx_mmap_verify_identity(&m, wrong, m.analyzer_id, m.analyzer_version, m.isa_id) == ORYX_ERR_INTEGRITY, "wrong module hash rejected");
	CHECK(oryx_mmap_verify_identity(&m, m.module_hash, 0xbadbad, m.analyzer_version, m.isa_id) == ORYX_ERR_INTEGRITY, "wrong analyzer id rejected");
	CHECK(oryx_mmap_verify_identity(&m, m.module_hash, m.analyzer_id, 999, m.isa_id) == ORYX_ERR_INTEGRITY, "wrong analyzer version rejected");
	CHECK(oryx_mmap_verify_identity(&m, m.module_hash, m.analyzer_id, m.analyzer_version, 0xdead) == ORYX_ERR_INTEGRITY, "wrong isa rejected");

	/* Wrong-identity cache get -> not found (different logical key). */
	CHECK(oryx_mmap_cache_get(&c, wrong, m.analyzer_id, m.analyzer_version, m.isa_id, &got) == ORYX_ERR_NOTFOUND,
	      "get with wrong hash -> NOTFOUND");

	/* Tamper the stored blob -> integrity rejection before use. */
	char blob[700]; snprintf(blob, sizeof(blob), "%s/blobs/%s", TESTDIR, addr);
	FILE *f = fopen(blob, "r+b");
	if (f) { fseek(f, 60, SEEK_SET); int ch = fgetc(f); fseek(f, 60, SEEK_SET); fputc(ch ^ 0xff, f); fclose(f); }
	CHECK(oryx_mmap_cache_get(&c, m.module_hash, m.analyzer_id, m.analyzer_version, m.isa_id, &got) == ORYX_ERR_INTEGRITY,
	      "tampered map blob rejected (content-address mismatch)");

	rm_rf(TESTDIR);
}

int main(void)
{
	test_decide_contract();
	test_format_and_lookup();
	test_parse_rejects();
	test_classifier();
	test_identity_and_cache();
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
