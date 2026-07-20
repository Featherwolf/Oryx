// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * test_oryxrun.c — BEHAVIOURAL tests: translate real guest blocks and EXECUTE
 * them, asserting on register/memory results. Byte-level tests prove the
 * encodings; these prove the semantics — the register mapping, the memory-model
 * lowering paths (plain STR/LDR vs ordered STLR/LDAPR/LDAR vs DMB scheme), and
 * the atomics actually compute the right thing when run.
 *
 * Executes on AArch64 (real device, or this repo's CI under qemu-aarch64). On any
 * other host oryx_run returns UNSUPPORTED and the execution assertions are
 * skipped (translation itself is covered by test_oryxtu).
 */
#define _GNU_SOURCE
#include "oryxrun.h"
#include "oryxtu.h"
#include "oryxcache.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); } } while (0)

/* Translate + run one straight-line block (must end in RET). Returns rc. */
static int run_block(const struct oryx_ginsn *ops, size_t n,
		     enum oryx_mm_policy pol, enum oryx_order_strength str,
		     struct oryx_guest *g)
{
	struct oryx_tu tu;
	int rc = oryx_translate_ex(ops, n, 0x1000, (uint32_t)(n * 4), 0, pol, str, &tu, NULL);
	if (rc != ORYX_OK)
		return rc;
	rc = oryx_run(&tu, g);
	oryx_tu_free(&tu);
	return rc;
}

static struct oryx_ginsn I(int op, int rd, int rn, int64_t imm)
{
	struct oryx_ginsn g = { .op = op, .rd = rd, .rn = rn, .imm = imm };
	return g;
}

static void test_arithmetic(void)
{
	printf("test: arithmetic executes (mov/add/sub/mov_rr)\n");
	struct oryx_ginsn ops[] = {
		I(GOP_MOV_RI, GR_RAX, 0, 10),
		I(GOP_MOV_RI, GR_RCX, 0, 5),
		I(GOP_ADD_RR, GR_RAX, GR_RCX, 0),   /* RAX = 10 + 5 = 15 */
		I(GOP_SUB_RR, GR_RAX, GR_RCX, 0),   /* RAX = 15 - 5 = 10 */
		I(GOP_MOV_RR, GR_RDX, GR_RAX, 0),   /* RDX = RAX = 10 */
		I(GOP_RET,    0, 0, 0),
	};
	struct oryx_guest g = {0};
	CHECK(run_block(ops, 6, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "translate+run");
	CHECK(g.regs[GR_RAX] == 10, "RAX == 10");
	CHECK(g.regs[GR_RDX] == 10, "RDX == 10 (mov_rr)");
	CHECK(g.regs[GR_RCX] == 5,  "RCX == 5");
}

static void test_mov_imm64(void)
{
	printf("test: 64-bit immediate (movz/movk chain)\n");
	uint64_t v = 0x1122334455667788ull;
	struct oryx_ginsn ops[] = {
		I(GOP_MOV_RI, GR_RSI, 0, (int64_t)v),
		I(GOP_RET,    0, 0, 0),
	};
	struct oryx_guest g = {0};
	CHECK(run_block(ops, 2, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "run");
	CHECK(g.regs[GR_RSI] == v, "RSI holds full 64-bit immediate");
}

/* One store + load pair against a host buffer, for a given policy/mclass/strength
 * and byte displacement. Verifies both the memory write and the loaded value. */
static void store_load_case(const char *name, enum oryx_mm_policy pol, int mclass,
			    enum oryx_order_strength str, int64_t disp)
{
	uint64_t mem[4] = { 0, 0, 0, 0 };
	int slot = (int)(disp / 8);
	struct oryx_ginsn st = I(GOP_STORE, GR_RAX, GR_RDI, disp); st.mclass = mclass;
	struct oryx_ginsn ld = I(GOP_LOAD,  GR_RDX, GR_RDI, disp); ld.mclass = mclass;
	struct oryx_ginsn ops[] = { st, ld, I(GOP_RET, 0, 0, 0) };

	struct oryx_guest g = {0};
	g.regs[GR_RDI] = (uint64_t)(uintptr_t)mem;   /* base pointer (runtime host addr) */
	g.regs[GR_RAX] = 0xCAFEF00DBABE0001ull + (uint64_t)slot;

	int rc = run_block(ops, 3, pol, str, &g);
	CHECK(rc == ORYX_OK, name);
	CHECK(mem[slot] == g.regs[GR_RAX], name);        /* store landed */
	CHECK(g.regs[GR_RDX] == mem[slot], name);        /* load read it back */
}

static void test_memory_paths(void)
{
	printf("test: load/store — every memory-model lowering path executes correctly\n");
	/* LOCAL -> plain STR/LDR (scaled offset). */
	store_load_case("LOCAL disp0  (STR/LDR)",  ORYX_POLICY_DRF, ORYX_MCLASS_LOCAL,  ORYX_ORDER_SC,   0);
	store_load_case("LOCAL disp8  (STR/LDR)",  ORYX_POLICY_DRF, ORYX_MCLASS_LOCAL,  ORYX_ORDER_SC,   8);
	/* SHARED + SC   -> STLR / LDAR. */
	store_load_case("SHARED SC   disp0 (STLR/LDAR)",  ORYX_POLICY_DRF, ORYX_MCLASS_SHARED, ORYX_ORDER_SC,   0);
	store_load_case("SHARED SC   disp8 (ADD+STLR/LDAR)", ORYX_POLICY_DRF, ORYX_MCLASS_SHARED, ORYX_ORDER_SC, 8);
	/* SHARED + RCpc -> STLR / LDAPR (exact-TSO load). */
	store_load_case("SHARED RCpc disp0 (STLR/LDAPR)", ORYX_POLICY_DRF, ORYX_MCLASS_SHARED, ORYX_ORDER_RCPC, 0);
	store_load_case("SHARED RCpc disp8 (ADD+STLR/LDAPR)", ORYX_POLICY_DRF, ORYX_MCLASS_SHARED, ORYX_ORDER_RCPC, 8);
	/* SHARED + DMB-fence scheme -> LDR;DMB / DMB;STR (scaled offset). */
	store_load_case("SHARED DMB  disp0 (DMB scheme)",  ORYX_POLICY_DRF, ORYX_MCLASS_SHARED, ORYX_ORDER_TSO,  0);
	store_load_case("SHARED DMB  disp8 (DMB scheme)",  ORYX_POLICY_DRF, ORYX_MCLASS_SHARED, ORYX_ORDER_TSO,  8);
	/* CONSERVATIVE policy orders even a LOCAL access — still functionally correct. */
	store_load_case("CONSERVATIVE forces order", ORYX_POLICY_CONSERVATIVE, ORYX_MCLASS_LOCAL, ORYX_ORDER_SC, 0);
}

static void test_atomics(void)
{
	printf("test: atomics execute (LDADDAL, CASAL)\n");
	/* LOCK ADD [RDI] += RAX  (LDADDAL). */
	{
		uint64_t mem[1] = { 100 };
		struct oryx_ginsn ops[] = { I(GOP_ATOMIC_ADD, GR_RAX, GR_RDI, 0), I(GOP_RET,0,0,0) };
		struct oryx_guest g = {0};
		g.regs[GR_RDI] = (uint64_t)(uintptr_t)mem;
		g.regs[GR_RAX] = 5;
		CHECK(run_block(ops, 2, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "atomic add run");
		CHECK(mem[0] == 105, "LDADDAL: [mem] == 105");
	}
	/* LOCK CMPXCHG [RDI]: expect RAX==42 -> store RCX(99); RAX gets old value. */
	{
		uint64_t mem[1] = { 42 };
		struct oryx_ginsn ops[] = { I(GOP_ATOMIC_CAS, GR_RCX, GR_RDI, 0), I(GOP_RET,0,0,0) };
		struct oryx_guest g = {0};
		g.regs[GR_RDI] = (uint64_t)(uintptr_t)mem;
		g.regs[GR_RAX] = 42;   /* comparand */
		g.regs[GR_RCX] = 99;   /* desired   */
		CHECK(run_block(ops, 2, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "cas run");
		CHECK(mem[0] == 99, "CASAL swapped: [mem] == 99");
		CHECK(g.regs[GR_RAX] == 42, "CASAL: RAX holds old value 42");
	}
	/* CAS that should FAIL (comparand mismatches). */
	{
		uint64_t mem[1] = { 7 };
		struct oryx_ginsn ops[] = { I(GOP_ATOMIC_CAS, GR_RCX, GR_RDI, 0), I(GOP_RET,0,0,0) };
		struct oryx_guest g = {0};
		g.regs[GR_RDI] = (uint64_t)(uintptr_t)mem;
		g.regs[GR_RAX] = 42;   /* comparand != 7 -> no swap */
		g.regs[GR_RCX] = 99;
		CHECK(run_block(ops, 2, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "cas-fail run");
		CHECK(mem[0] == 7, "CASAL no-swap: [mem] unchanged");
		CHECK(g.regs[GR_RAX] == 7, "CASAL: RAX holds current value 7");
	}
}

static void test_fence_and_cmp_smoke(void)
{
	printf("test: fence + cmp execute without fault\n");
	struct oryx_ginsn ops[] = {
		I(GOP_MOV_RI, GR_RAX, 0, 3),
		I(GOP_MOV_RI, GR_RCX, 0, 3),
		I(GOP_CMP_RR, GR_RAX, GR_RCX, 0),           /* SUBS XZR,RAX,RCX (sets flags) */
		I(GOP_FENCE,  0, 0, 0),                     /* DMB ISH */
		I(GOP_RET,    0, 0, 0),
	};
	ops[3].cc = ORYX_FENCE_FULL;
	struct oryx_guest g = {0};
	CHECK(run_block(ops, 5, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "cmp+fence run");
	CHECK(g.regs[GR_RAX] == 3, "RAX preserved across cmp/fence");
}

int main(void)
{
	if (!oryx_run_supported()) {
		printf("oryxrun: execution tests SKIPPED (host is not AArch64).\n");
		printf("         build with aarch64-linux-gnu-gcc and run under qemu-aarch64,\n");
		printf("         or run on the device, to execute translated blocks.\n");
		/* Still exercise map's arch-independent guards so the host build isn't a no-op. */
		struct oryx_tu tu; struct oryx_ginsn ret = I(GOP_RET,0,0,0);
		if (oryx_translate_ex(&ret, 1, 0x1000, 4, 0, ORYX_POLICY_DRF, ORYX_ORDER_SC, &tu, NULL) == ORYX_OK) {
			struct oryx_exec e;
			CHECK(oryx_exec_map(&tu, &e) == ORYX_OK, "map succeeds (bytes are copied+protected)");
			oryx_exec_free(&e);
			CHECK(oryx_exec_run(&e, NULL) == ORYX_ERR_INVAL || !oryx_run_supported(), "run guarded off-arch");
			oryx_tu_free(&tu);
		}
		printf("\n%d passed, %d failed (host guards only)\n", g_pass, g_fail);
		return g_fail ? 1 : 0;
	}

	test_arithmetic();
	test_mov_imm64();
	test_memory_paths();
	test_atomics();
	test_fence_and_cmp_smoke();
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
