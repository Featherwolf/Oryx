/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * oryxrun.h — minimal EXECUTION runtime for oryx_tu blocks.
 *
 * liboryxtu turns a guest basic block into position-independent AArch64 bytes
 * with a fixed guest->host register mapping (guest reg N -> host xN, N in 0..15;
 * x16 scratch; x30 link). This library makes those bytes actually RUN: it maps
 * them executable (W^X), loads a guest register file into x0..x15, jumps in, and
 * reads the registers back. It is the bridge from "byte-correct mapping" to
 * "executes x86 semantics on ARM", and it turns the encoder/mapping tests from
 * byte comparisons into behavioural ones.
 *
 * Scope (this build): straight-line blocks that end in RET — arithmetic, moves,
 * loads/stores (plain and ordered), atomics, fences. Guest LOAD/STORE use a
 * guest register as the base address, interpreted as a HOST address (a flat,
 * identity guest<->host address space): put a real pointer in the base register
 * and the block reads/writes that memory. Multi-block control-flow dispatch
 * (resolving branch relocations, a guest-PC->host-block map) is the next build.
 *
 * Address-space caveat: because guest and host addresses are identified, the
 * runtime's OWN writable state — in particular the struct oryx_guest register
 * file passed to oryx_exec_run — is guest-reachable. Guest code that computes a
 * base address aliasing that register file would observe/clobber it mid-run
 * (the trampoline holds x0..x15 in CPU registers across the block, so a store
 * into regs[] does not take effect until after RET). Callers must keep guest
 * memory disjoint from the oryx_guest they pass in. A real VM gives the guest a
 * separate address window; this identity-mapped reference runtime does not.
 *
 * Executing AArch64 requires an AArch64 host (real device, or this repo's tests
 * under qemu-aarch64). On any other arch the map/run calls return
 * ORYX_ERR_UNSUPPORTED so the library still builds and links everywhere.
 */
#ifndef ORYXRUN_H
#define ORYXRUN_H

#include <stddef.h>
#include <stdint.h>
#include "oryxcache.h"   /* struct oryx_tu, error codes */
#include "oryxtu.h"      /* GR_COUNT (guest register count) */

#ifdef __cplusplus
extern "C" {
#endif

/* Guest architectural state the runtime loads/stores around a block call.
 * regs[i] is guest GPR i (== host xi for i in 0..GR_COUNT-1). */
struct oryx_guest {
	uint64_t regs[GR_COUNT];
};

/* A mapped, executable copy of a TU's code. Opaque-ish; free with oryx_exec_free. */
struct oryx_exec {
	void   *code;      /* executable mapping base (entry point) */
	size_t  map_len;   /* full mapping length (for munmap) */
	uint32_t code_len; /* bytes of actual code */
};

/* Returned in addition to the ORYX_ERR_* set in oryxcache.h. */
#ifndef ORYX_ERR_UNSUPPORTED
#define ORYX_ERR_UNSUPPORTED (-100)   /* cannot execute AArch64 on this host    */
#endif
#ifndef ORYX_ERR_PERM
#define ORYX_ERR_PERM        (-101)   /* exec mapping denied by W^X/SELinux policy */
#endif

/*
 * Map a TU's code into an executable region (mmap RW -> memcpy -> mprotect RX ->
 * clear icache). Arch-independent (it only copies + protects bytes), so it
 * succeeds on any POSIX host — the bytes are just not runnable off AArch64;
 * oryx_exec_run is the arch-gated part. Fails with ORYX_ERR_INVAL on a
 * NULL/empty TU, one with unresolved branch relocations (this build executes
 * only straight-line blocks: reloc_count must be 0), or one not ending in RET;
 * ORYX_ERR_FORMAT if code_len is not a multiple of 4; ORYX_ERR_PERM if the exec
 * mapping is denied by policy; ORYX_ERR_NOMEM on other mmap/mprotect failure.
 */
int  oryx_exec_map(const struct oryx_tu *tu, struct oryx_exec *out);
void oryx_exec_free(struct oryx_exec *e);

/*
 * Run an already-mapped block against a guest register file: load g->regs[0..15]
 * into x0..x15, call the block (which ends in RET), then write x0..x15 back into
 * g->regs. Host callee-saved state is preserved. AArch64 only.
 */
int  oryx_exec_run(const struct oryx_exec *e, struct oryx_guest *g);

/*
 * Convenience: map + run + free. Most callers want this. `g` is updated in place.
 */
int  oryx_run(const struct oryx_tu *tu, struct oryx_guest *g);

/* True if this build can actually execute AArch64 blocks (i.e. host is AArch64). */
int  oryx_run_supported(void);

#ifdef __cplusplus
}
#endif

#endif /* ORYXRUN_H */
