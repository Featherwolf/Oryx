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
	printf("test: atomics execute (LDADDAL, CASAL) — full 64-bit operands\n");
	/* LOCK ADD [RDI] += RAX. 64-bit operands so a 32-bit-encoding regression
	 * (which would leave the high word untouched) produces a wrong result. */
	{
		uint64_t mem[1] = { 0x1122334455667788ull };
		struct oryx_ginsn ops[] = { I(GOP_ATOMIC_ADD, GR_RAX, GR_RDI, 0), I(GOP_RET,0,0,0) };
		struct oryx_guest g = {0};
		g.regs[GR_RDI] = (uint64_t)(uintptr_t)mem;
		g.regs[GR_RAX] = 0x0000000500000005ull;     /* adds into BOTH words */
		CHECK(run_block(ops, 2, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "atomic add run");
		CHECK(mem[0] == 0x112233495566778Dull, "LDADDAL: full 64-bit add (high word carried too)");
	}
	/* LOCK CMPXCHG [RDI], match: comparand==[mem] over all 64 bits -> swap in RCX;
	 * RAX gets the full old value. A 32-bit CAS would mis-handle the high word. */
	{
		uint64_t mem[1] = { 0xDEADBEEFCAFE0099ull };
		struct oryx_ginsn ops[] = { I(GOP_ATOMIC_CAS, GR_RCX, GR_RDI, 0), I(GOP_RET,0,0,0) };
		struct oryx_guest g = {0};
		g.regs[GR_RDI] = (uint64_t)(uintptr_t)mem;
		g.regs[GR_RAX] = 0xDEADBEEFCAFE0099ull;      /* comparand (full match) */
		g.regs[GR_RCX] = 0x1234567800000042ull;      /* desired */
		CHECK(run_block(ops, 2, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "cas-match run");
		CHECK(mem[0] == 0x1234567800000042ull, "CASAL match: [mem] = full 64-bit desired");
		CHECK(g.regs[GR_RAX] == 0xDEADBEEFCAFE0099ull, "CASAL match: RAX = full 64-bit old value");
	}
	/* CAS mismatch where the LOW 32 bits are EQUAL but the high words differ: a
	 * correct 64-bit CASAL must NOT swap; a 32-bit regression wrongly would. */
	{
		uint64_t mem[1] = { 0xDEADBEEF00000042ull };
		struct oryx_ginsn ops[] = { I(GOP_ATOMIC_CAS, GR_RCX, GR_RDI, 0), I(GOP_RET,0,0,0) };
		struct oryx_guest g = {0};
		g.regs[GR_RDI] = (uint64_t)(uintptr_t)mem;
		g.regs[GR_RAX] = 0x1111111100000042ull;      /* low32 matches, high32 differs */
		g.regs[GR_RCX] = 0x9999999999999999ull;
		CHECK(run_block(ops, 2, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "cas-mismatch run");
		CHECK(mem[0] == 0xDEADBEEF00000042ull, "CASAL mismatch (high word): NO swap");
		CHECK(g.regs[GR_RAX] == 0xDEADBEEF00000042ull, "CASAL mismatch: RAX = full 64-bit current value");
	}
}

/* SETcc materializes a flag into a register — the only way to OBSERVE the flags
 * CMP computes in a straight-line (branch-free) block. This makes CMP's flag
 * semantics behaviourally tested, not just byte-tested. */
static void setcc_case(int64_t a, int64_t b, int expect_eq, int expect_lt)
{
	struct oryx_ginsn ops[6];
	ops[0] = I(GOP_MOV_RI, GR_RAX, 0, a);
	ops[1] = I(GOP_MOV_RI, GR_RCX, 0, b);
	ops[2] = I(GOP_CMP_RR, GR_RAX, GR_RCX, 0);          /* flags = a - b */
	ops[3] = (struct oryx_ginsn){ .op = GOP_SETCC, .rd = GR_RDX, .cc = GCC_EQ };
	ops[4] = (struct oryx_ginsn){ .op = GOP_SETCC, .rd = GR_RSI, .cc = GCC_LT };
	ops[5] = I(GOP_RET, 0, 0, 0);
	struct oryx_guest g = {0};
	CHECK(run_block(ops, 6, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "cmp+setcc run");
	CHECK((int)g.regs[GR_RDX] == expect_eq, "SETcc EQ matches CMP flags");
	CHECK((int)g.regs[GR_RSI] == expect_lt, "SETcc LT matches CMP flags");
}

static void test_flags_and_fence(void)
{
	printf("test: CMP flags observed via SETcc + fence executes\n");
	setcc_case(3, 3, 1, 0);    /* 3 - 3 == 0 -> EQ true,  LT false */
	setcc_case(3, 5, 0, 1);    /* 3 - 5  < 0 -> EQ false, LT true  */
	setcc_case(9, 4, 0, 0);    /* 9 - 4  > 0 -> EQ false, LT false */

	/* Fence executes without fault and preserves surrounding register state. */
	struct oryx_ginsn f[3];
	f[0] = I(GOP_MOV_RI, GR_RAX, 0, 0x7);
	f[1] = (struct oryx_ginsn){ .op = GOP_FENCE, .cc = ORYX_FENCE_FULL };  /* DMB ISH */
	f[2] = I(GOP_RET, 0, 0, 0);
	struct oryx_guest g = {0};
	CHECK(run_block(f, 3, ORYX_POLICY_DRF, ORYX_ORDER_SC, &g) == ORYX_OK, "fence run");
	CHECK(g.regs[GR_RAX] == 0x7, "RAX preserved across DMB");
}

int main(void)
{
	if (!oryx_run_supported()) {
		printf("oryxrun: execution tests SKIPPED (host is not AArch64).\n");
		printf("         build with aarch64-linux-gnu-gcc and run under qemu-aarch64,\n");
		printf("         or run on the device, to execute translated blocks.\n");
		/* Still exercise the arch-independent paths so the host build isn't a no-op:
		 * map (which copies+protects), the RET-terminator guard, and that run is
		 * properly gated to UNSUPPORTED off AArch64. */
		struct oryx_tu tu; struct oryx_ginsn ret = I(GOP_RET,0,0,0);
		struct oryx_guest gd = {0};
		if (oryx_translate_ex(&ret, 1, 0x1000, 4, 0, ORYX_POLICY_DRF, ORYX_ORDER_SC, &tu, NULL) == ORYX_OK) {
			struct oryx_exec e;
			CHECK(oryx_exec_map(&tu, &e) == ORYX_OK, "map succeeds (RET-terminated block)");
			CHECK(oryx_exec_run(&e, &gd) == ORYX_ERR_UNSUPPORTED, "run returns UNSUPPORTED off AArch64");
			oryx_exec_free(&e);
			oryx_tu_free(&tu);
		}
		printf("\n%d passed, %d failed (host guards only)\n", g_pass, g_fail);
		return g_fail ? 1 : 0;
	}

	test_arithmetic();
	test_mov_imm64();
	test_memory_paths();
	test_atomics();
	test_flags_and_fence();
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
