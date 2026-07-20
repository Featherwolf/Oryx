// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryxrun.c — execution runtime for oryx_tu blocks. Maps the translated AArch64
 * bytes executable (W^X) and runs them against a guest register file.
 */
#define _GNU_SOURCE
#include "oryxrun.h"

#include <string.h>
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

#if defined(__aarch64__)
/*
 * Trampoline: load guest regs[0..15] into x0..x15, BLR into the block (which
 * ends in RET, returning here), then store x0..x15 back. The two pointers are
 * pinned to callee-saved x19/x20, which the block never touches (it uses only
 * x0..x16 + x30), so they survive the call. Everything the block can clobber is
 * listed so the compiler saves nothing live across the asm in those registers.
 * x18 is the platform register — deliberately not touched.
 */
int oryx_exec_run(const struct oryx_exec *e, struct oryx_guest *g)
{
	if (!e || !e->code || !g)
		return ORYX_ERR_INVAL;

	register uint64_t   *rp __asm__("x19") = g->regs;
	register const void *cp __asm__("x20") = e->code;

	__asm__ __volatile__(
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
		: [r] "+r"(rp), [c] "+r"(cp)
		:
		: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
		  "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x30",
		  "cc", "memory");
	return ORYX_OK;
}
#else
int oryx_exec_run(const struct oryx_exec *e, struct oryx_guest *g)
{
	(void)e; (void)g;
	return ORYX_ERR_UNSUPPORTED;   /* cannot execute AArch64 on this host */
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
