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
#define ORYX_ERR_UNSUPPORTED (-100)
#endif

/*
 * Map a TU's code into an executable region (mmap RW -> memcpy -> mprotect RX ->
 * clear icache). Fails with ORYX_ERR_INVAL on a NULL/empty or unresolved-branch
 * TU (this build executes only straight-line blocks: reloc_count must be 0),
 * ORYX_ERR_UNSUPPORTED off AArch64, ORYX_ERR_NOMEM on mmap/mprotect failure.
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
