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
 * Address-space caveat: because guest and host addresses are identified, ALL of
 * the runtime's own host allocations are guest-reachable — a guest LOAD/STORE
 * whose base aliases them observes/clobbers runtime state. This includes: the
 * struct oryx_guest register file (held in CPU registers across a block, so a
 * store into regs[] lands only after RET); the dispatcher's block cache (which
 * holds host code pointers the dispatcher BLRs into — corrupting one escalates
 * from data clobber to control-flow hijack); and the PROT_READ|EXEC code pages
 * (a store there faults). The register file the caller can keep disjoint; the
 * cache and code pages live at runtime-chosen addresses the caller cannot
 * predict. A real VM gives the guest a separate address window; this identity-
 * mapped reference runtime does not, so it is for trusted guest code only.
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
#ifndef ORYX_ERR_STEPS
#define ORYX_ERR_STEPS       (-102)   /* oryx_run_program hit its max_steps guard  */
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

/* ---- multi-block control-flow dispatch --------------------------------------
 *
 * Runs a whole guest PROGRAM, not one block: translate the block at the current
 * guest PC, execute it, take the successor PC it reports, and repeat — a lazy
 * translate-on-demand dispatcher with a block cache. Branches, conditionals, and
 * loops all work because a control-flow block reports which successor it took.
 *
 * Mechanism (return-to-dispatcher, no inter-block chaining): oryx_exec_map_linked
 * appends small "exit stubs" after a block's body; each terminator branch is
 * repointed to the stub for its target, and the stub loads that target guest PC
 * into x17 and RETs to the dispatcher. A block that ends in guest RET writes no
 * x17 and so reports ORYX_HALT_PC, stopping the program. (Guest CALL/RET with a
 * real return stack is not modelled yet: RET means "program done".)
 */

/* Successor-PC sentinel meaning "stop": a guest RET reports this. ~0 is reserved
 * as a guest PC target for this reason. */
#define ORYX_HALT_PC  ((uint64_t)~0ull)

/* Max guest ops the dispatcher will fetch per block. */
#define ORYX_MAX_BLOCK_OPS 64

/*
 * Fetch the guest IR for the basic block at `pc`. Fill `ops` (capacity `cap`),
 * set `*n` (op count) and `*len` (guest bytes covered). Return ORYX_OK, or an
 * ORYX_ERR_* (e.g. ORYX_ERR_NOTFOUND) if there is no block at `pc` — which stops
 * the program with that error. `ctx` is the opaque pointer passed to
 * oryx_run_program.
 */
typedef int (*oryx_fetch_fn)(uint64_t pc, struct oryx_ginsn *ops, size_t cap,
			     size_t *n, uint32_t *len, void *ctx);

/*
 * Map a block executable AND link its control flow: append exit stubs and
 * repoint the terminator branch(es) at them. Accepts blocks ending in RET (no
 * stubs), JMP (one stub), or BR (two stubs — taken + fall-through). Same error
 * contract as oryx_exec_map, plus ORYX_ERR_INVAL for >1 relocation.
 */
int  oryx_exec_map_linked(const struct oryx_tu *tu, struct oryx_exec *out);

/*
 * Run the program starting at `entry_pc` against guest state `g`, using `fetch`
 * to obtain IR on demand and translating with (policy, strength). Stops when a
 * block reports ORYX_HALT_PC (guest RET); returns ORYX_ERR_STEPS if it runs
 * longer than `max_steps` (0 = a built-in default runaway guard), or ORYX_ERR_*
 * from fetch/translate/map. `*steps_out` (nullable) receives the number of
 * blocks executed. Translated blocks are cached in a bounded hash table; when
 * the cache fills it is flushed wholesale (blocks retranslate on next use), so
 * live executable memory stays bounded regardless of how many distinct guest
 * PCs the program reaches. AArch64 only.
 */
int  oryx_run_program(oryx_fetch_fn fetch, void *ctx,
		      enum oryx_mm_policy policy, enum oryx_order_strength strength,
		      uint64_t entry_pc, struct oryx_guest *g,
		      uint64_t max_steps, uint64_t *steps_out);

#ifdef __cplusplus
}
#endif

#endif /* ORYXRUN_H */
