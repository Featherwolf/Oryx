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

/* ---- multi-block dispatcher tests --------------------------------------- */

/* Guest program: a countdown loop that sums the counter.
 *   0x1000 (loop): RAX += RCX ; RCX -= RBX ; CMP RCX,RDX ; BR NE -> 0x1000 (else 0x2000)
 *   0x2000 (exit): RET
 * With RAX=0,RCX=5,RBX=1,RDX=0 -> RAX = 5+4+3+2+1 = 15, exits when RCX hits 0. */
static int fetch_sumloop(uint64_t pc, struct oryx_ginsn *ops, size_t cap,
			 size_t *n, uint32_t *len, void *ctx)
{
	(void)ctx;
	if (cap < 4) return ORYX_ERR_INVAL;
	if (pc == 0x1000) {
		size_t k = 0;
		ops[k++] = I(GOP_ADD_RR, GR_RAX, GR_RCX, 0);   /* sum += counter   */
		ops[k++] = I(GOP_SUB_RR, GR_RCX, GR_RBX, 0);   /* counter -= 1     */
		ops[k++] = I(GOP_CMP_RR, GR_RCX, GR_RDX, 0);   /* flags = RCX - 0  */
		ops[k++] = (struct oryx_ginsn){ .op = GOP_BR, .cc = GCC_NE, .target = 0x1000 };
		*n = k; *len = 0x1000;                         /* fall-through = 0x2000 */
		return ORYX_OK;
	}
	if (pc == 0x2000) {
		ops[0] = I(GOP_RET, 0, 0, 0);
		*n = 1; *len = 4;
		return ORYX_OK;
	}
	return ORYX_ERR_NOTFOUND;
}

/* Guest program: a 40-block chain, each block RAX += RCX then JMP next; block 40
 * is RET. Exercises the hash-table block cache across many distinct guest PCs. */
#define LC_BASE 0x10000ull
#define LC_N    40u
static int fetch_longchain(uint64_t pc, struct oryx_ginsn *ops, size_t cap,
			   size_t *n, uint32_t *len, void *ctx)
{
	(void)ctx; (void)cap;
	if (pc >= LC_BASE && pc < LC_BASE + LC_N * 0x100ull && ((pc - LC_BASE) % 0x100ull) == 0) {
		ops[0] = I(GOP_ADD_RR, GR_RAX, GR_RCX, 0);                       /* RAX += 1 */
		ops[1] = (struct oryx_ginsn){ .op = GOP_JMP, .target = pc + 0x100ull };
		*n = 2; *len = 0x100; return ORYX_OK;
	}
	if (pc == LC_BASE + LC_N * 0x100ull) { ops[0] = I(GOP_RET, 0, 0, 0); *n = 1; *len = 4; return ORYX_OK; }
	return ORYX_ERR_NOTFOUND;
}

/* Guest program: an unconditional JMP chain. 0x100 -> 0x200 -> 0x300 (RET). */
static int fetch_jmpchain(uint64_t pc, struct oryx_ginsn *ops, size_t cap,
			  size_t *n, uint32_t *len, void *ctx)
{
	(void)ctx; (void)cap;
	if (pc == 0x100) {
		ops[0] = I(GOP_MOV_RI, GR_RAX, 0, 42);
		ops[1] = (struct oryx_ginsn){ .op = GOP_JMP, .target = 0x200 };
		*n = 2; *len = 8; return ORYX_OK;
	}
	if (pc == 0x200) {
		ops[0] = I(GOP_ADD_RR, GR_RAX, GR_RAX, 0);     /* RAX += RAX -> 84 */
		ops[1] = (struct oryx_ginsn){ .op = GOP_JMP, .target = 0x300 };
		*n = 2; *len = 8; return ORYX_OK;
	}
	if (pc == 0x300) {
		ops[0] = I(GOP_RET, 0, 0, 0);
		*n = 1; *len = 4; return ORYX_OK;
	}
	return ORYX_ERR_NOTFOUND;
}

static void test_dispatcher(void)
{
	printf("test: multi-block dispatcher (loop + conditional, JMP chain)\n");
	/* Loop with a conditional branch. */
	{
		struct oryx_guest g = {0};
		g.regs[GR_RAX] = 0; g.regs[GR_RCX] = 5; g.regs[GR_RBX] = 1; g.regs[GR_RDX] = 0;
		uint64_t steps = 0;
		int rc = oryx_run_program(fetch_sumloop, NULL, ORYX_POLICY_DRF, ORYX_ORDER_SC,
					  0x1000, &g, 1000, &steps);
		CHECK(rc == ORYX_OK, "sum-loop ran to halt");
		CHECK(g.regs[GR_RAX] == 15, "RAX = 5+4+3+2+1 = 15 (loop ran 5x)");
		CHECK(g.regs[GR_RCX] == 0, "counter reached 0");
		CHECK(steps == 6, "6 blocks executed (5 loop bodies + exit)");
	}
	/* Unconditional JMP chain across three blocks. */
	{
		struct oryx_guest g = {0};
		uint64_t steps = 0;
		int rc = oryx_run_program(fetch_jmpchain, NULL, ORYX_POLICY_DRF, ORYX_ORDER_SC,
					  0x100, &g, 1000, &steps);
		CHECK(rc == ORYX_OK, "jmp-chain ran to halt");
		CHECK(g.regs[GR_RAX] == 84, "RAX = 42 then doubled = 84");
		CHECK(steps == 3, "3 blocks executed (0x100 -> 0x200 -> 0x300)");
	}
	/* Runaway guard: an infinite loop must be stopped by max_steps. */
	{
		struct oryx_guest g = {0};
		g.regs[GR_RAX] = 0; g.regs[GR_RCX] = 1; g.regs[GR_RBX] = 0; g.regs[GR_RDX] = 0;
		/* RBX=0 so RCX never decrements -> CMP RCX,0 stays NE -> infinite loop. */
		uint64_t steps = 0;
		int rc = oryx_run_program(fetch_sumloop, NULL, ORYX_POLICY_DRF, ORYX_ORDER_SC,
					  0x1000, &g, 50, &steps);
		CHECK(rc == ORYX_ERR_STEPS, "runaway loop stopped by max_steps (distinct code)");
		CHECK(steps == 50, "stopped exactly at the step cap");
	}
	/* Missing block: fetch returns NOTFOUND -> program stops with that error. */
	{
		struct oryx_guest g = {0};
		int rc = oryx_run_program(fetch_jmpchain, NULL, ORYX_POLICY_DRF, ORYX_ORDER_SC,
					  0x999, &g, 1000, NULL);
		CHECK(rc == ORYX_ERR_NOTFOUND, "unknown entry PC -> NOTFOUND");
	}
	/* 40-block chain: many distinct guest PCs through the hash cache. */
	{
		struct oryx_guest g = {0};
		g.regs[GR_RAX] = 0; g.regs[GR_RCX] = 1;
		uint64_t steps = 0;
		int rc = oryx_run_program(fetch_longchain, NULL, ORYX_POLICY_DRF, ORYX_ORDER_SC,
					  LC_BASE, &g, 1000, &steps);
		CHECK(rc == ORYX_OK, "40-block chain ran to halt");
		CHECK(g.regs[GR_RAX] == LC_N, "RAX incremented once per block = 40");
		CHECK(steps == LC_N + 1, "41 blocks executed (40 + RET)");
	}
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
	test_dispatcher();
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
