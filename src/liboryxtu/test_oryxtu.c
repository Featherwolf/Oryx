// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * test_oryxtu.c — unit tests for the deterministic reference translator.
 * Proves: real AArch64 encodings, byte-determinism, control-flow relocations,
 * well-formedness checks, and end-to-end integration with liboryxcache.
 */
#define _GNU_SOURCE
#include "oryxtu.h"
#include "oryxcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, msg) do { \
	if (cond) g_pass++; \
	else { g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static uint32_t word_at(const struct oryx_tu *tu, uint32_t byte_off)
{
	const uint8_t *p = tu->code + byte_off;
	return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

/* A representative basic block:
 *   RAX = 0x1234
 *   RAX += RCX
 *   cmp RAX, RBX        (flags = RAX - RBX)
 *   je   0x4000
 */
static size_t sample_block(struct oryx_ginsn *b)
{
	size_t n = 0;
	b[n++] = (struct oryx_ginsn){ .op = GOP_MOV_RI, .rd = GR_RAX, .imm = 0x1234 };
	b[n++] = (struct oryx_ginsn){ .op = GOP_ADD_RR, .rd = GR_RAX, .rn = GR_RCX };
	b[n++] = (struct oryx_ginsn){ .op = GOP_CMP_RR, .rd = GR_RAX, .rn = GR_RBX };
	b[n++] = (struct oryx_ginsn){ .op = GOP_BR, .cc = GCC_EQ, .target = 0x4000 };
	return n;
}

static void test_encodings(void)
{
	printf("test: real AArch64 encodings (known-answer)\n");
	struct oryx_ginsn b[8];
	size_t n = sample_block(b);
	struct oryx_tu tu;
	CHECK(oryx_translate(b, n, 0x400000, 16, 3, &tu) == ORYX_OK, "translate sample");

	/* MOVZ X0,#0x1234 ; ADD X0,X0,X1 ; SUBS XZR,X0,X3 ; B.EQ +0 */
	CHECK(word_at(&tu, 0)  == 0xD2824680u, "MOVZ X0,#0x1234");
	CHECK(word_at(&tu, 4)  == 0x8B010000u, "ADD X0,X0,X1");
	CHECK(word_at(&tu, 8)  == 0xEB03001Fu, "SUBS XZR,X0,X3 (cmp)");
	CHECK(word_at(&tu, 12) == 0x54000000u, "B.EQ +0 (reloc-patched later)");
	CHECK(tu.code_len == 16, "4 instructions => 16 bytes");
	oryx_tu_free(&tu);

	/* MOV X1,X2 and RET as isolated checks. */
	struct oryx_ginsn mv[2] = {
		{ .op = GOP_MOV_RR, .rd = GR_RCX, .rn = GR_RDX },  /* x1 = x2 */
		{ .op = GOP_RET },
	};
	struct oryx_tu t2;
	oryx_translate(mv, 2, 0x1000, 4, 0, &t2);
	CHECK(word_at(&t2, 0) == 0xAA0203E1u, "ORR X1,XZR,X2 (mov)");
	CHECK(word_at(&t2, 4) == 0xD65F03C0u, "RET");
	oryx_tu_free(&t2);
}

static void test_relocs_exits(void)
{
	printf("test: control-flow relocations and exits\n");
	struct oryx_ginsn b[8];
	size_t n = sample_block(b);
	struct oryx_tu tu;
	oryx_translate(b, n, 0x400000, 16, 3, &tu);

	CHECK(tu.reloc_count == 1, "one relocation (the branch)");
	CHECK(tu.relocs[0].offset == 12, "reloc at the B.cond offset");
	CHECK(tu.relocs[0].kind == ORYX_RELOC_BRANCH_GUEST_PC, "branch reloc kind");
	CHECK(tu.relocs[0].guest_target == 0x4000, "reloc target = 0x4000");
	CHECK(tu.exit_count == 2, "two exits: taken + fallthrough");
	CHECK(tu.exits[0] == 0x4000 && tu.exits[1] == 0x400000 + 16,
	      "exits are branch target and fallthrough");
	oryx_tu_free(&tu);
}

static void test_determinism(void)
{
	printf("test: byte-deterministic output\n");
	struct oryx_ginsn b[8];
	size_t n = sample_block(b);

	struct oryx_tu a, c;
	oryx_translate(b, n, 0x400000, 16, 3, &a);
	oryx_translate(b, n, 0x400000, 16, 3, &c);

	uint8_t *sa, *sc; size_t la, lc;
	oryx_tu_serialize(&a, &sa, &la);
	oryx_tu_serialize(&c, &sc, &lc);
	CHECK(la == lc && memcmp(sa, sc, la) == 0, "two translations are byte-identical");

	char addr_a[65], addr_c[65];
	oryx_content_address(sa, la, addr_a);
	oryx_content_address(sc, lc, addr_c);
	CHECK(strcmp(addr_a, addr_c) == 0, "identical content address");

	/* A different profile_id must change identity (it is part of the key/header). */
	struct oryx_tu d;
	oryx_translate(b, n, 0x400000, 16, 4, &d);
	char key_a[65], key_d[65];
	oryx_tu_logical_key(&a, key_a);
	oryx_tu_logical_key(&d, key_d);
	CHECK(strcmp(key_a, key_d) != 0, "different profile_id => different logical key");

	free(sa); free(sc);
	oryx_tu_free(&a); oryx_tu_free(&c); oryx_tu_free(&d);
}

static void test_cache_integration(void)
{
	printf("test: translate -> cache -> retrieve -> verify\n");
	if (system("rm -rf ./_tucache") != 0) { /* ignore */ }
	struct oryx_cache cache;
	CHECK(oryx_cache_open(&cache, "./_tucache") == ORYX_OK, "open cache");

	struct oryx_ginsn b[8];
	size_t n = sample_block(b);
	struct oryx_tu tu;
	oryx_translate(b, n, 0x400000, 16, 3, &tu);

	char addr[65], key[65];
	oryx_tu_logical_key(&tu, key);
	CHECK(oryx_cache_put_tu(&cache, &tu, addr) == ORYX_OK, "put translated TU");

	struct oryx_tu got;
	CHECK(oryx_cache_get_tu(&cache, key, &got) == ORYX_OK, "get by logical key");
	CHECK(got.guest_entry_pc == 0x400000, "retrieved entry pc");
	CHECK(got.code_len == tu.code_len &&
	      memcmp(got.code, tu.code, tu.code_len) == 0, "retrieved code identical");
	CHECK(got.reloc_count == 1 && got.relocs[0].guest_target == 0x4000,
	      "retrieved relocation intact");

	oryx_tu_free(&got);
	oryx_tu_free(&tu);
	if (system("rm -rf ./_tucache") != 0) { /* ignore */ }
}

static void test_wellformed(void)
{
	printf("test: well-formedness checks\n");
	struct oryx_tu tu;

	/* Code after a terminator is rejected. */
	struct oryx_ginsn bad[3] = {
		{ .op = GOP_RET },
		{ .op = GOP_MOV_RR, .rd = GR_RAX, .rn = GR_RCX },
		{ .op = GOP_RET },
	};
	CHECK(oryx_translate(bad, 3, 0x1000, 4, 0, &tu) == ORYX_ERR_INVAL,
	      "reject code after terminator");

	/* Missing terminator is rejected. */
	struct oryx_ginsn noterm[1] = { { .op = GOP_ADD_RR, .rd = GR_RAX, .rn = GR_RCX } };
	CHECK(oryx_translate(noterm, 1, 0x1000, 4, 0, &tu) == ORYX_ERR_INVAL,
	      "reject block with no terminator");

	/* Misaligned load displacement is rejected. */
	struct oryx_ginsn badld[2] = {
		{ .op = GOP_LOAD, .rd = GR_RAX, .rn = GR_RDI, .imm = 7 },  /* not 8-aligned */
		{ .op = GOP_RET },
	};
	CHECK(oryx_translate(badld, 2, 0x1000, 8, 0, &tu) == ORYX_ERR_INVAL,
	      "reject misaligned load displacement");

	/* A valid load/store block succeeds and encodes LDR/STR. */
	struct oryx_ginsn okld[3] = {
		{ .op = GOP_LOAD,  .rd = GR_RAX, .rn = GR_RDI, .imm = 16 },
		{ .op = GOP_STORE, .rd = GR_RAX, .rn = GR_RSI, .imm = 8 },
		{ .op = GOP_RET },
	};
	CHECK(oryx_translate(okld, 3, 0x1000, 12, 0, &tu) == ORYX_OK, "valid load/store block");
	/* LDR X0,[X7,#16] : imm12=2 -> 0xF9400000 | (2<<10) | (7<<5) | 0 */
	CHECK(word_at(&tu, 0) == (0xF9400000u | (2u<<10) | (7u<<5)), "LDR X0,[X7,#16]");
	/* STR X0,[X6,#8]  : imm12=1 -> 0xF9000000 | (1<<10) | (6<<5) | 0 */
	CHECK(word_at(&tu, 4) == (0xF9000000u | (1u<<10) | (6u<<5)), "STR X0,[X6,#8]");
	oryx_tu_free(&tu);
}

static void test_memory_model(void)
{
	printf("test: Door 3 memory-model lowering (ordered/atomic encodings)\n");
	struct oryx_ginsn b[8]; size_t n = 0;
	b[n++] = (struct oryx_ginsn){ .op = GOP_LOAD,  .rd = GR_RAX, .rn = GR_RDI, .imm = 0, .mclass = ORYX_MCLASS_SHARED };
	b[n++] = (struct oryx_ginsn){ .op = GOP_STORE, .rd = GR_RAX, .rn = GR_RSI, .imm = 0, .mclass = ORYX_MCLASS_SHARED };
	b[n++] = (struct oryx_ginsn){ .op = GOP_ATOMIC_ADD, .rd = GR_RCX, .rn = GR_RDI };
	b[n++] = (struct oryx_ginsn){ .op = GOP_ATOMIC_CAS, .rd = GR_RDX, .rn = GR_RSI };
	b[n++] = (struct oryx_ginsn){ .op = GOP_FENCE, .cc = ORYX_FENCE_FULL };
	b[n++] = (struct oryx_ginsn){ .op = GOP_RET };

	struct oryx_tu tu; struct oryx_mm_stats st;
	CHECK(oryx_translate_ex(b, n, 0x2000, 24, 3, ORYX_POLICY_DRF, ORYX_ORDER_SC, &tu, &st) == ORYX_OK,
	      "translate_ex ok");
	CHECK(word_at(&tu, 0)  == 0xC8DFFCE0u, "LDAR X0,[X7]  (shared load -> acquire)");
	CHECK(word_at(&tu, 4)  == 0xC89FFCC0u, "STLR X0,[X6]  (shared store -> release)");
	CHECK(word_at(&tu, 8)  == 0xF8E100FFu, "LDADDAL X1,XZR,[X7]  (LOCK ADD)");
	CHECK(word_at(&tu, 12) == 0xC8E0FCC2u, "CASAL X0,X2,[X6]  (LOCK CMPXCHG)");
	CHECK(word_at(&tu, 16) == 0xD5033BBFu, "DMB ISH  (MFENCE)");
	CHECK(st.ordered_loads == 1 && st.ordered_stores == 1 &&
	      st.atomics == 2 && st.fences == 1, "stats counted ordered+atomic+fence");
	CHECK(st.plain_loads == 0 && st.plain_stores == 0, "no weak accesses in this block");
	oryx_tu_free(&tu);
}

static void test_barrier_reduction(void)
{
	printf("test: DRF policy emits far fewer barriers than conservative\n");
	struct oryx_ginsn b[8]; size_t n = 0;
	/* 3 thread-local stack accesses + 2 genuinely-shared + 1 atomic. */
	b[n++] = (struct oryx_ginsn){ .op = GOP_STORE, .rd = GR_RAX, .rn = GR_RSP, .imm = 8,  .mclass = ORYX_MCLASS_LOCAL };
	b[n++] = (struct oryx_ginsn){ .op = GOP_LOAD,  .rd = GR_RCX, .rn = GR_RSP, .imm = 16, .mclass = ORYX_MCLASS_LOCAL };
	b[n++] = (struct oryx_ginsn){ .op = GOP_STORE, .rd = GR_RDX, .rn = GR_RBP, .imm = 8,  .mclass = ORYX_MCLASS_LOCAL };
	b[n++] = (struct oryx_ginsn){ .op = GOP_LOAD,  .rd = GR_RSI, .rn = GR_RDI, .imm = 0,  .mclass = ORYX_MCLASS_SHARED };
	b[n++] = (struct oryx_ginsn){ .op = GOP_STORE, .rd = GR_RSI, .rn = GR_R8,  .imm = 0,  .mclass = ORYX_MCLASS_SHARED };
	b[n++] = (struct oryx_ginsn){ .op = GOP_ATOMIC_ADD, .rd = GR_RCX, .rn = GR_RDI };
	b[n++] = (struct oryx_ginsn){ .op = GOP_RET };

	struct oryx_tu drf, con; struct oryx_mm_stats sd, sc;
	CHECK(oryx_translate_ex(b, n, 0x3000, 28, 1, ORYX_POLICY_DRF, ORYX_ORDER_SC, &drf, &sd) == ORYX_OK, "drf translate");
	CHECK(oryx_translate_ex(b, n, 0x3000, 28, 1, ORYX_POLICY_CONSERVATIVE, ORYX_ORDER_SC, &con, &sc) == ORYX_OK, "conservative translate");

	uint32_t drf_ordered = sd.ordered_loads + sd.ordered_stores;
	uint32_t con_ordered = sc.ordered_loads + sc.ordered_stores;
	printf("      DRF ordered=%u weak=%u | CONSERVATIVE ordered=%u weak=%u\n",
	       drf_ordered, sd.plain_loads + sd.plain_stores,
	       con_ordered, sc.plain_loads + sc.plain_stores);
	CHECK(drf_ordered == 2, "DRF orders only the 2 shared accesses");
	CHECK(con_ordered == 5, "conservative orders all 5 ordinary accesses");
	CHECK(drf_ordered < con_ordered, "DRF < conservative (fewer barriers = the win)");
	CHECK(sd.plain_loads + sd.plain_stores == 3, "DRF runs the 3 stack accesses weak/free");
	CHECK(sc.plain_loads + sc.plain_stores == 0, "conservative runs nothing weak");
	CHECK(sd.atomics == 1 && sc.atomics == 1, "the real atomic stays ordered under both");
	oryx_tu_free(&drf); oryx_tu_free(&con);
}

static void test_exact_tso_mapping(void)
{
	printf("test: exact-TSO DMB-fence mapping (the corrected minimal mapping)\n");
	struct oryx_ginsn b[4]; size_t n = 0;
	b[n++] = (struct oryx_ginsn){ .op = GOP_LOAD,  .rd = GR_RAX, .rn = GR_RDI, .imm = 0, .mclass = ORYX_MCLASS_SHARED };
	b[n++] = (struct oryx_ginsn){ .op = GOP_STORE, .rd = GR_RAX, .rn = GR_RSI, .imm = 8, .mclass = ORYX_MCLASS_SHARED };
	b[n++] = (struct oryx_ginsn){ .op = GOP_RET };

	struct oryx_tu tu; struct oryx_mm_stats st;
	CHECK(oryx_translate_ex(b, n, 0x5000, 16, 2, ORYX_POLICY_DRF, ORYX_ORDER_TSO, &tu, &st) == ORYX_OK,
	      "exact-TSO translate ok");
	/* SHARED load  -> LDR X0,[X7] ; DMB ISHLD   (trailing read fence) */
	CHECK(word_at(&tu, 0)  == 0xF94000E0u, "LDR X0,[X7]");
	CHECK(word_at(&tu, 4)  == 0xD50339BFu, "DMB ISHLD (after load)");
	/* SHARED store -> DMB ISHST ; STR X0,[X6,#8] (leading write fence) */
	CHECK(word_at(&tu, 8)  == 0xD5033ABFu, "DMB ISHST (before store)");
	CHECK(word_at(&tu, 12) == 0xF90004C0u, "STR X0,[X6,#8]");
	CHECK(word_at(&tu, 16) == 0xD65F03C0u, "RET");
	CHECK(st.ordered_loads == 1 && st.ordered_stores == 1, "counted as ordered");

	/* The same SHARED access under SC strength lowers to LDAR — proving the modes differ. */
	struct oryx_tu sc;
	oryx_translate_ex(b, n, 0x5000, 16, 2, ORYX_POLICY_DRF, ORYX_ORDER_SC, &sc, NULL);
	CHECK(word_at(&sc, 0) == 0xC8DFFCE0u, "SC strength uses LDAR X0,[X7] instead (contrast)");
	oryx_tu_free(&tu); oryx_tu_free(&sc);
}

static void test_rcpc_mapping(void)
{
	printf("test: RCpc mapping (LDAPR/STLR = cheapest exact-TSO on FEAT_LRCPC)\n");
	struct oryx_ginsn b[4]; size_t n = 0;
	b[n++] = (struct oryx_ginsn){ .op = GOP_LOAD,  .rd = GR_RAX, .rn = GR_RDI, .imm = 0, .mclass = ORYX_MCLASS_SHARED };
	b[n++] = (struct oryx_ginsn){ .op = GOP_STORE, .rd = GR_RAX, .rn = GR_RSI, .imm = 0, .mclass = ORYX_MCLASS_SHARED };
	b[n++] = (struct oryx_ginsn){ .op = GOP_RET };

	struct oryx_tu tu;
	CHECK(oryx_translate_ex(b, n, 0x6000, 16, 4, ORYX_POLICY_DRF, ORYX_ORDER_RCPC, &tu, NULL) == ORYX_OK, "rcpc translate");
	CHECK(word_at(&tu, 0) == 0xF8BFC0E0u, "LDAPR X0,[X7] (exact-TSO acquire load)");
	CHECK(word_at(&tu, 4) == 0xC89FFCC0u, "STLR X0,[X6]  (release store)");
	CHECK(word_at(&tu, 8) == 0xD65F03C0u, "RET");
	oryx_tu_free(&tu);

	/* The three ordered strengths must produce three DIFFERENT content addresses
	 * (proves the guest hash folds in `strength` — the cache-aliasing fix). */
	struct oryx_tu sc, tso; uint8_t *xa, *xb, *xc; size_t la, lb, lc; char aa[65], ab[65], ac[65];
	oryx_translate_ex(b, n, 0x6000, 16, 4, ORYX_POLICY_DRF, ORYX_ORDER_RCPC, &tu, NULL);
	oryx_translate_ex(b, n, 0x6000, 16, 4, ORYX_POLICY_DRF, ORYX_ORDER_SC, &sc, NULL);
	oryx_translate_ex(b, n, 0x6000, 16, 4, ORYX_POLICY_DRF, ORYX_ORDER_TSO, &tso, NULL);
	oryx_tu_serialize(&tu, &xa, &la); oryx_tu_serialize(&sc, &xb, &lb); oryx_tu_serialize(&tso, &xc, &lc);
	oryx_content_address(xa, la, aa); oryx_content_address(xb, lb, ab); oryx_content_address(xc, lc, ac);
	CHECK(strcmp(aa, ab) != 0 && strcmp(aa, ac) != 0 && strcmp(ab, ac) != 0,
	      "RCPC/SC/TSO strengths => distinct content addresses (no aliasing)");
	free(xa); free(xb); free(xc);
	oryx_tu_free(&tu); oryx_tu_free(&sc); oryx_tu_free(&tso);
}

static void test_hash_folds_mclass(void)
{
	printf("test: guest hash folds in mclass (cache-aliasing regression)\n");
	struct oryx_ginsn local[2] = {
		{ .op = GOP_LOAD, .rd = GR_RAX, .rn = GR_RDI, .imm = 0, .mclass = ORYX_MCLASS_LOCAL },
		{ .op = GOP_RET },
	};
	struct oryx_ginsn shared[2] = {
		{ .op = GOP_LOAD, .rd = GR_RAX, .rn = GR_RDI, .imm = 0, .mclass = ORYX_MCLASS_SHARED },
		{ .op = GOP_RET },
	};
	struct oryx_tu a, b; char ka[65], kb[65];
	oryx_translate(local, 2, 0x7000, 8, 5, &a);   /* DRF: LOCAL -> weak LDR */
	oryx_translate(shared, 2, 0x7000, 8, 5, &b);  /* DRF: SHARED -> ordered LDAR */
	CHECK(word_at(&a, 0) != word_at(&b, 0), "the two blocks really do emit different code");
	oryx_tu_logical_key(&a, ka);
	oryx_tu_logical_key(&b, kb);
	CHECK(strcmp(ka, kb) != 0, "different mclass => different logical key (NO collision)");
	oryx_tu_free(&a); oryx_tu_free(&b);
}

int main(void)
{
	test_encodings();
	test_relocs_exits();
	test_determinism();
	test_cache_integration();
	test_wellformed();
	test_memory_model();
	test_barrier_reduction();
	test_exact_tso_mapping();
	test_rcpc_mapping();
	test_hash_folds_mclass();
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
