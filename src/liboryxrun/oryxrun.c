// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryxrun.c — execution runtime for oryx_tu blocks. Maps the translated AArch64
 * bytes executable (W^X) and runs them against a guest register file.
 */
#define _GNU_SOURCE
#include "oryxrun.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

/* AArch64 "RET x30" — every executable straight-line block must end in this. */
#define AARCH64_RET 0xD65F03C0u

int oryx_run_supported(void)
{
#if defined(__aarch64__)
	return 1;
#else
	return 0;
#endif
}

int oryx_exec_map(const struct oryx_tu *tu, struct oryx_exec *out)
{
	if (!tu || !tu->code || tu->code_len == 0 || !out)
		return ORYX_ERR_INVAL;
	/* This build executes only straight-line blocks (end in RET); a block with
	 * unresolved branch relocations needs the multi-block dispatcher. */
	if (tu->reloc_count != 0)
		return ORYX_ERR_INVAL;
	/* Code is a stream of 32-bit AArch64 instructions. */
	if ((tu->code_len & 3u) != 0)
		return ORYX_ERR_FORMAT;
	/* A straight-line block is entered via BLR and must return via RET, else
	 * execution falls off the end of the mapping. The translator guarantees a
	 * terminator; verify it defensively so a malformed/hand-built TU can never
	 * make the runtime run into arbitrary bytes. */
	{
		const uint8_t *last = tu->code + tu->code_len - 4;
		uint32_t insn = (uint32_t)last[0] | ((uint32_t)last[1] << 8) |
				((uint32_t)last[2] << 16) | ((uint32_t)last[3] << 24);
		if (insn != AARCH64_RET)
			return ORYX_ERR_INVAL;
	}

	long pg = sysconf(_SC_PAGESIZE);
	size_t page = (pg > 0) ? (size_t)pg : 4096u;
	size_t map_len = ((size_t)tu->code_len + page - 1) & ~(page - 1);

	/* W^X: map writable, copy, then flip to read+execute. */
	void *m = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED)
		return ORYX_ERR_NOMEM;
	memcpy(m, tu->code, tu->code_len);
	if (mprotect(m, map_len, PROT_READ | PROT_EXEC) != 0) {
		int e = errno;                 /* capture before munmap clobbers it */
		munmap(m, map_len);
		/* On W^X-hardened targets (SELinux execmem, PR_SET_MDWE, PaX) this
		 * fails with EACCES/EPERM — a policy problem, not out-of-memory. */
		return (e == EACCES || e == EPERM) ? ORYX_ERR_PERM : ORYX_ERR_NOMEM;
	}
	__builtin___clear_cache((char *)m, (char *)m + tu->code_len);

	out->code = m;
	out->map_len = map_len;
	out->code_len = tu->code_len;
	return ORYX_OK;
}

void oryx_exec_free(struct oryx_exec *e)
{
	if (e && e->code) {
		munmap(e->code, e->map_len);
		e->code = NULL;
		e->map_len = 0;
		e->code_len = 0;
	}
}

/* ---- control-flow linking (arch-independent: it just patches bytes) ------- */
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static void wr32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

#define STUB_REG   17u          /* x17 = the dispatcher's next-guest-PC register */
#define STUB_BYTES 20u          /* MOVZ + 3x MOVK + RET (fixed size, any 64-bit PC) */

/* Emit "MOVZ/MOVK x17, <pc> ; RET" — an exit stub that hands the successor guest
 * PC back to the dispatcher. Always 4 mov + ret so the size is PC-independent. */
static void emit_setpc_ret(uint8_t *dst, uint64_t pc)
{
	wr32(dst + 0,  0xD2800000u | ((uint32_t)( pc         & 0xffff) << 5) | STUB_REG);            /* MOVZ x17,#..,LSL 0  */
	wr32(dst + 4,  0xF2800000u | (1u<<21) | ((uint32_t)((pc >> 16) & 0xffff) << 5) | STUB_REG);  /* MOVK x17,#..,LSL 16 */
	wr32(dst + 8,  0xF2800000u | (2u<<21) | ((uint32_t)((pc >> 32) & 0xffff) << 5) | STUB_REG);  /* MOVK x17,#..,LSL 32 */
	wr32(dst + 12, 0xF2800000u | (3u<<21) | ((uint32_t)((pc >> 48) & 0xffff) << 5) | STUB_REG);  /* MOVK x17,#..,LSL 48 */
	wr32(dst + 16, 0xD65F03C0u);                                                                 /* RET                 */
}

/* Repoint the B / B.cond at byte offset `site_off` to byte offset `to_off`
 * (both within the same mapping, so always in range). Preserves B.cond's cond. */
static void patch_branch(uint8_t *base, uint32_t site_off, uint32_t to_off)
{
	uint8_t *site = base + site_off;
	uint32_t insn = rd32(site);
	int32_t  imm  = ((int32_t)to_off - (int32_t)site_off) / 4;
	if ((insn & 0xFF000000u) == 0x14000000u)                 /* B (unconditional) */
		wr32(site, 0x14000000u | ((uint32_t)imm & 0x03FFFFFFu));
	else                                                     /* B.cond */
		wr32(site, 0x54000000u | (((uint32_t)imm & 0x7FFFFu) << 5) | (insn & 0xfu));
}

int oryx_exec_map_linked(const struct oryx_tu *tu, struct oryx_exec *out)
{
	if (!tu || !tu->code || tu->code_len == 0 || !out)
		return ORYX_ERR_INVAL;
	if ((tu->code_len & 3u) != 0)
		return ORYX_ERR_FORMAT;
	if (tu->reloc_count > 1)
		return ORYX_ERR_INVAL;               /* one terminator branch, at most */

	unsigned nstub = 0;
	if (tu->reloc_count == 1) {
		const struct oryx_reloc *r = &tu->relocs[0];
		if (r->kind != ORYX_RELOC_BRANCH_GUEST_PC ||
		    r->offset + 4u != tu->code_len)   /* the branch must be the last insn */
			return ORYX_ERR_FORMAT;
		nstub = (tu->exit_count >= 2) ? 2u : 1u;   /* BR: taken+fallthrough; JMP: taken */
		if (!tu->exits || tu->exit_count < 1)
			return ORYX_ERR_FORMAT;
	} else {
		const uint8_t *last = tu->code + tu->code_len - 4;
		if (rd32(last) != 0xD65F03C0u)        /* reloc-free block must end in RET */
			return ORYX_ERR_INVAL;
	}

	size_t total   = (size_t)tu->code_len + (size_t)nstub * STUB_BYTES;
	long   pgc     = sysconf(_SC_PAGESIZE);
	size_t page    = (pgc > 0) ? (size_t)pgc : 4096u;
	size_t map_len = (total + page - 1) & ~(page - 1);

	uint8_t *m = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED)
		return ORYX_ERR_NOMEM;
	memcpy(m, tu->code, tu->code_len);

	if (nstub == 1) {                         /* JMP: one taken stub after the body */
		uint32_t stub = tu->code_len;
		emit_setpc_ret(m + stub, tu->relocs[0].guest_target);
		patch_branch(m, tu->relocs[0].offset, stub);
	} else if (nstub == 2) {                  /* BR: fall-through then taken stub */
		uint32_t ft = tu->code_len;                 /* not-taken falls in here */
		uint32_t tk = tu->code_len + STUB_BYTES;    /* B.cond retargeted here   */
		emit_setpc_ret(m + ft, tu->exits[1]);       /* fall-through PC */
		emit_setpc_ret(m + tk, tu->relocs[0].guest_target); /* taken PC */
		patch_branch(m, tu->relocs[0].offset, tk);
	}

	if (mprotect(m, map_len, PROT_READ | PROT_EXEC) != 0) {
		int e = errno;
		munmap(m, map_len);
		return (e == EACCES || e == EPERM) ? ORYX_ERR_PERM : ORYX_ERR_NOMEM;
	}
	__builtin___clear_cache((char *)m, (char *)m + total);

	out->code = m;
	out->map_len = map_len;
	out->code_len = (uint32_t)total;
	return ORYX_OK;
}

#if defined(__aarch64__)
/*
 * Trampoline: seed x17 with the HALT sentinel, load guest regs[0..15] into
 * x0..x15, BLR into the block, then store x0..x15 back and return x17.
 *
 * x17 is the block's "next guest PC" out-register. A straight-line/RET block
 * never writes x17, so it returns HALT. A control-flow block reaches an exit
 * stub (appended by oryx_exec_map_linked) that sets x17 = successor guest PC and
 * RETs. The block body only ever touches x0..x16 + x30, so x17, x19, x20 (our
 * pointers) survive the call; x18 (platform register) is left alone.
 */
static uint64_t oryx_tramp(uint64_t *regs, const void *code)
{
	register uint64_t   *rp  __asm__("x19") = regs;
	register const void *cp  __asm__("x20") = code;
	register uint64_t    nxt __asm__("x17");

	__asm__ __volatile__(
		"movn x17, #0               \n"   /* x17 = HALT sentinel (~0)   */
		"ldp x0,  x1,  [%[r], #0]   \n"
		"ldp x2,  x3,  [%[r], #16]  \n"
		"ldp x4,  x5,  [%[r], #32]  \n"
		"ldp x6,  x7,  [%[r], #48]  \n"
		"ldp x8,  x9,  [%[r], #64]  \n"
		"ldp x10, x11, [%[r], #80]  \n"
		"ldp x12, x13, [%[r], #96]  \n"
		"ldp x14, x15, [%[r], #112] \n"
		"blr %[c]                   \n"
		"stp x0,  x1,  [%[r], #0]   \n"
		"stp x2,  x3,  [%[r], #16]  \n"
		"stp x4,  x5,  [%[r], #32]  \n"
		"stp x6,  x7,  [%[r], #48]  \n"
		"stp x8,  x9,  [%[r], #64]  \n"
		"stp x10, x11, [%[r], #80]  \n"
		"stp x12, x13, [%[r], #96]  \n"
		"stp x14, x15, [%[r], #112] \n"
		: [r] "+r"(rp), [c] "+r"(cp), [n] "=r"(nxt)
		:
		: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
		  "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x30",
		  "cc", "memory");
	return nxt;
}

int oryx_exec_run(const struct oryx_exec *e, struct oryx_guest *g)
{
	if (!e || !e->code || !g)
		return ORYX_ERR_INVAL;
	(void)oryx_tramp(g->regs, e->code);   /* single block: ignore next-PC */
	return ORYX_OK;
}

/* The dispatcher: lazily translate+link+cache the block at each guest PC, run
 * it, follow the successor PC it reports, until a block reports ORYX_HALT_PC. */
struct oryx_centry { uint64_t pc; struct oryx_exec e; };

int oryx_run_program(oryx_fetch_fn fetch, void *ctx,
		     enum oryx_mm_policy pol, enum oryx_order_strength str,
		     uint64_t entry_pc, struct oryx_guest *g,
		     uint64_t max_steps, uint64_t *steps_out)
{
	if (!fetch || !g)
		return ORYX_ERR_INVAL;
	if (max_steps == 0)
		max_steps = 10000000ull;             /* runaway guard default */

	struct oryx_centry *cache = NULL;
	size_t ncache = 0, ccache = 0;
	uint64_t pc = entry_pc, steps = 0;
	int rc = ORYX_OK;

	while (pc != ORYX_HALT_PC) {
		if (steps >= max_steps) { rc = ORYX_ERR_INVAL; break; }

		void *code = NULL;
		for (size_t i = 0; i < ncache; i++)
			if (cache[i].pc == pc) { code = cache[i].e.code; break; }

		if (!code) {
			struct oryx_ginsn ops[ORYX_MAX_BLOCK_OPS];
			size_t n = 0; uint32_t len = 0;
			rc = fetch(pc, ops, ORYX_MAX_BLOCK_OPS, &n, &len, ctx);
			if (rc != ORYX_OK) break;
			struct oryx_tu tu;
			rc = oryx_translate_ex(ops, n, pc, len, 0, pol, str, &tu, NULL);
			if (rc != ORYX_OK) break;
			struct oryx_exec e;
			rc = oryx_exec_map_linked(&tu, &e);
			oryx_tu_free(&tu);
			if (rc != ORYX_OK) break;
			if (ncache == ccache) {
				size_t nc = ccache ? ccache * 2 : 8;
				struct oryx_centry *nn = realloc(cache, nc * sizeof(*nn));
				if (!nn) { oryx_exec_free(&e); rc = ORYX_ERR_NOMEM; break; }
				cache = nn; ccache = nc;
			}
			cache[ncache].pc = pc; cache[ncache].e = e; ncache++;
			code = e.code;                       /* stable mmap base */
		}

		pc = oryx_tramp(g->regs, code);
		steps++;
	}

	for (size_t i = 0; i < ncache; i++)
		oryx_exec_free(&cache[i].e);
	free(cache);
	if (steps_out) *steps_out = steps;
	return rc;
}
#else
int oryx_exec_run(const struct oryx_exec *e, struct oryx_guest *g)
{
	(void)e; (void)g;
	return ORYX_ERR_UNSUPPORTED;   /* cannot execute AArch64 on this host */
}
int oryx_run_program(oryx_fetch_fn fetch, void *ctx,
		     enum oryx_mm_policy pol, enum oryx_order_strength str,
		     uint64_t entry_pc, struct oryx_guest *g,
		     uint64_t max_steps, uint64_t *steps_out)
{
	(void)fetch; (void)ctx; (void)pol; (void)str; (void)entry_pc;
	(void)g; (void)max_steps; (void)steps_out;
	return ORYX_ERR_UNSUPPORTED;
}
#endif

int oryx_run(const struct oryx_tu *tu, struct oryx_guest *g)
{
	struct oryx_exec e;
	int rc = oryx_exec_map(tu, &e);
	if (rc != ORYX_OK)
		return rc;
	rc = oryx_exec_run(&e, g);
	oryx_exec_free(&e);
	return rc;
}
