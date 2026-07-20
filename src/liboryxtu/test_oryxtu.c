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

int main(void)
{
	test_encodings();
	test_relocs_exits();
	test_determinism();
	test_cache_integration();
	test_wellformed();
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
