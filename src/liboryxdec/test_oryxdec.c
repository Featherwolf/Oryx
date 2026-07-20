// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * test_oryxdec.c — decode REAL gcc -O1 compiled x86-64 functions and (under
 * qemu-aarch64) execute them, asserting the results match the C originals.
 *
 * The byte arrays below are the exact .text of these functions, emitted by
 *   gcc -O1 -fno-stack-protector:
 *     long sum_to_n(long n){ long s=0; for(long i=1;i<=n;i++) s+=i; return s; }
 *     long fib(long n){ long a=0,b=1; while(n-->0){ long t=a+b; a=b; b=t; } return a; }
 * Decode correctness is checked on any host; execution runs on AArch64/qemu.
 */
#define _GNU_SOURCE
#include "oryxdec.h"
#include "oryxrun.h"
#include "oryxtu.h"
#include "oryxcache.h"

#include <stdio.h>
#include <stdint.h>

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); } } while (0)

/* gcc -O1 -fno-stack-protector: long sum_to_n(long n) */
static const uint8_t sum_to_n_code[] = {
	0xf3, 0x0f, 0x1e, 0xfa, 0x48, 0x85, 0xff, 0x7e, 0x1e, 0x48, 0x83, 0xc7,
	0x01, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xba, 0x00, 0x00, 0x00, 0x00, 0x48,
	0x01, 0xc2, 0x48, 0x83, 0xc0, 0x01, 0x48, 0x39, 0xf8, 0x75, 0xf4, 0x48,
	0x89, 0xd0, 0xc3, 0xba, 0x00, 0x00, 0x00, 0x00, 0xeb, 0xf5,
};
/* gcc -O1 -fno-stack-protector: long fib(long n) */
static const uint8_t fib_code[] = {
	0xf3, 0x0f, 0x1e, 0xfa, 0x48, 0x8d, 0x47, 0xff, 0x48, 0x85, 0xff, 0x7e,
	0x21, 0xba, 0x01, 0x00, 0x00, 0x00, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x48,
	0x89, 0xd1, 0x48, 0x01, 0xf2, 0x48, 0x83, 0xe8, 0x01, 0x48, 0x89, 0xce,
	0x48, 0x83, 0xf8, 0xff, 0x75, 0xed, 0x48, 0x89, 0xc8, 0xc3, 0xb9, 0x00,
	0x00, 0x00, 0x00, 0xeb, 0xf5,
};

#define GBASE 0x400000ull

/* ---- decode-level checks (arch-independent) ----------------------------- */
static void test_decode(void)
{
	printf("test: x86-64 decoder produces the right IR\n");
	struct oryx_x86_image img = { sum_to_n_code, sizeof(sum_to_n_code), GBASE };
	struct oryx_ginsn ops[16];
	size_t n = 0; uint32_t len = 0;

	/* Entry block: endbr64 (skipped) ; test %rdi,%rdi ; jle 0x27. */
	int rc = oryx_x86_decode_block(&img, GBASE, ops, 16, &n, &len);
	CHECK(rc == ORYX_OK, "decode entry block");
	CHECK(n == 2, "entry block: TEST + BR (endbr64 skipped)");
	CHECK(len == 9, "entry block spans 9 bytes (4+3+2)");
	CHECK(ops[0].op == GOP_TEST_RR && ops[0].rd == GR_RDI && ops[0].rn == GR_RDI, "TEST rdi,rdi");
	CHECK(ops[1].op == GOP_BR && ops[1].cc == GCC_LE && ops[1].target == GBASE + 0x27, "JLE -> 0x27");

	/* Loop body at 0x17: add %rax,%rdx ; add $1,%rax ; cmp %rdi,%rax ; jne 0x17. */
	rc = oryx_x86_decode_block(&img, GBASE + 0x17, ops, 16, &n, &len);
	CHECK(rc == ORYX_OK, "decode loop block");
	CHECK(n == 4, "loop block: ADD_RR, ADD_RI, CMP_RR, BR");
	CHECK(ops[0].op == GOP_ADD_RR && ops[0].rd == GR_RDX && ops[0].rn == GR_RAX, "add %rax,%rdx");
	CHECK(ops[1].op == GOP_ADD_RI && ops[1].rd == GR_RAX && ops[1].imm == 1, "add $1,%rax");
	CHECK(ops[2].op == GOP_CMP_RR && ops[2].rd == GR_RAX && ops[2].rn == GR_RDI, "cmp %rdi,%rax");
	CHECK(ops[3].op == GOP_BR && ops[3].cc == GCC_NE && ops[3].target == GBASE + 0x17, "jne -> 0x17");

	/* fib uses LEA and 64-bit immediates — check the LEA decode. */
	struct oryx_x86_image fimg = { fib_code, sizeof(fib_code), GBASE };
	rc = oryx_x86_decode_block(&fimg, GBASE, ops, 16, &n, &len);
	CHECK(rc == ORYX_OK, "decode fib entry");
	CHECK(ops[0].op == GOP_LEA && ops[0].rd == GR_RAX && ops[0].rn == GR_RDI && ops[0].imm == -1, "lea -1(%rdi),%rax");

	/* Out-of-range PC. */
	CHECK(oryx_x86_decode_block(&img, GBASE + 1000, ops, 16, &n, &len) == ORYX_ERR_NOTFOUND, "OOB pc -> NOTFOUND");
}

/* ---- execution checks (AArch64/qemu) ------------------------------------ */
static long run_fn(const uint8_t *code, size_t code_len, long arg)
{
	struct oryx_x86_image img = { code, code_len, GBASE };
	struct oryx_guest g = {0};
	g.regs[GR_RDI] = (uint64_t)arg;             /* System V: first arg in RDI */
	uint64_t steps = 0;
	int rc = oryx_run_program(oryx_x86_fetch, &img, ORYX_POLICY_DRF, ORYX_ORDER_SC,
				  GBASE, &g, 1000000, &steps);
	if (rc != ORYX_OK) return -0x0BADBEEF;
	return (long)g.regs[GR_RAX];                /* System V: return in RAX */
}

static void test_execute(void)
{
	printf("test: EXECUTE the decoded compiled functions\n");
	CHECK(run_fn(sum_to_n_code, sizeof(sum_to_n_code), 0)   == 0,    "sum_to_n(0) = 0");
	CHECK(run_fn(sum_to_n_code, sizeof(sum_to_n_code), 1)   == 1,    "sum_to_n(1) = 1");
	CHECK(run_fn(sum_to_n_code, sizeof(sum_to_n_code), 10)  == 55,   "sum_to_n(10) = 55");
	CHECK(run_fn(sum_to_n_code, sizeof(sum_to_n_code), 100) == 5050, "sum_to_n(100) = 5050");
	CHECK(run_fn(sum_to_n_code, sizeof(sum_to_n_code), -5)  == 0,    "sum_to_n(-5) = 0 (early return)");

	CHECK(run_fn(fib_code, sizeof(fib_code), 0)  == 0,  "fib(0) = 0");
	CHECK(run_fn(fib_code, sizeof(fib_code), 1)  == 1,  "fib(1) = 1");
	CHECK(run_fn(fib_code, sizeof(fib_code), 10) == 55, "fib(10) = 55");
	/* 64-bit overflow value — proves full 64-bit arithmetic end to end. */
	CHECK((uint64_t)run_fn(fib_code, sizeof(fib_code), 100) == 3736710778780434371ull, "fib(100) = full 64-bit overflow value");
}

int main(void)
{
	test_decode();
	if (oryx_run_supported()) {
		test_execute();
	} else {
		printf("(execution tests skipped: host is not AArch64 — run under qemu-aarch64)\n");
	}
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
