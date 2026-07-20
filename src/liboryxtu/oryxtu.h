/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * oryxtu.h — reference DETERMINISTIC x86-64 -> AArch64 basic-block translator.
 *
 * This is not Box64. It is a small, correct, byte-deterministic translator that
 * (a) proves the Part B pipeline end to end (translate -> serialize -> cache ->
 * verify) and (b) pins down the determinism contract that Box64/FEX codegen must
 * satisfy to be cacheable (see docs/partB-determinism.md).
 *
 * It lowers a guest basic block, expressed as a small register-transfer IR, to
 * position-independent AArch64 with a FIXED register mapping and relocations for
 * control-flow targets — no host addresses, no timestamps, no RNG — so identical
 * input always yields byte-identical output, on any device and any run.
 *
 * The IR is the codegen input on purpose: instruction *decoding* is not the
 * determinism risk; *lowering* (register allocation, constant handling, block
 * layout) is, and that is exactly what this models.
 */
#ifndef ORYXTU_H
#define ORYXTU_H

#include <stddef.h>
#include <stdint.h>
#include "oryxcache.h"   /* struct oryx_tu, error codes */

#ifdef __cplusplus
extern "C" {
#endif

/* Guest x86-64 GPRs (index doubles as the fixed AArch64 target register). */
enum oryx_greg {
	GR_RAX = 0, GR_RCX, GR_RDX, GR_RBX, GR_RSP, GR_RBP, GR_RSI, GR_RDI,
	GR_R8, GR_R9, GR_R10, GR_R11, GR_R12, GR_R13, GR_R14, GR_R15,
	GR_COUNT
};

/* Condition codes (x86 semantics; mapped to AArch64 B.cond conditions). */
enum oryx_gcc { GCC_EQ = 0, GCC_NE, GCC_LT, GCC_GE, GCC_LE, GCC_GT };

/* Guest IR opcodes. */
enum oryx_gop {
	GOP_MOV_RR = 0,  /* rd = rn                          */
	GOP_MOV_RI,      /* rd = imm                         */
	GOP_ADD_RR,      /* rd = rd + rn                     */
	GOP_SUB_RR,      /* rd = rd - rn                     */
	GOP_CMP_RR,      /* flags = rd - rn (x86 CMP dst,src); SUBS XZR,Xrd,Xrn */
	GOP_LOAD,        /* rd = [rn + imm]  (imm: 0..32760, 8-aligned) */
	GOP_STORE,       /* [rn + imm] = rd                  */
	GOP_BR,          /* if (cc) goto target ; terminates */
	GOP_JMP,         /* goto target      ; terminates    */
	GOP_RET,         /* return           ; terminates    */
	/* --- Door 3: synchronization (always ordered, policy-independent) --- */
	GOP_ATOMIC_ADD,  /* LOCK ADD [rn], rd   -> LDADDAL (acq+rel), disp 0    */
	GOP_ATOMIC_CAS,  /* LOCK CMPXCHG [rn]    -> CASAL (RAX=cmp, rd=desired)  */
	GOP_FENCE,       /* MFENCE/LFENCE/SFENCE -> DMB ISH/ISHLD/ISHST (via cc) */
	GOP_SETCC,       /* rd = (cc) ? 1 : 0  (x86 SETcc) -> CSET Xrd, cond     */
	/* --- integer forms the x86-64 decoder emits (imm sign-extended to 64) --- */
	GOP_ADD_RI,      /* rd = rd + imm                    -> ADD/SUB #imm      */
	GOP_SUB_RI,      /* rd = rd - imm                    -> SUB/ADD #imm      */
	GOP_CMP_RI,      /* flags = rd - imm                 -> SUBS/ADDS XZR     */
	GOP_TEST_RR,     /* flags = rd & rn                  -> ANDS XZR (TST)    */
	GOP_XOR_RR,      /* rd = rd ^ rn                     -> EOR               */
	GOP_AND_RR,      /* rd = rd & rn                     -> AND               */
	GOP_OR_RR,       /* rd = rd | rn                     -> ORR               */
	GOP_LEA          /* rd = rn + imm  (x86 LEA base+disp) -> ADD/SUB #imm    */
};

/*
 * Memory-ordering class for ordinary loads/stores (Door 3). LOCAL is the default
 * (0): provably thread-local memory (stack/TLS) that no other thread can observe,
 * lowered to plain weak LDR/STR. SHARED is potentially-shared memory, lowered to
 * TSO-equivalent acquire/release (LDAR/STLR).
 */
enum oryx_mclass { ORYX_MCLASS_LOCAL = 0, ORYX_MCLASS_SHARED = 1 };

/* Fence kinds for GOP_FENCE (carried in the `cc` field). */
enum oryx_fence { ORYX_FENCE_FULL = 0, ORYX_FENCE_LD = 1, ORYX_FENCE_ST = 2 };

struct oryx_ginsn {
	int      op;      /* enum oryx_gop */
	int      rd;      /* enum oryx_greg */
	int      rn;      /* enum oryx_greg */
	int64_t  imm;     /* MOV_RI value, or LOAD/STORE displacement */
	uint64_t target;  /* guest PC for BR/JMP */
	int      cc;      /* enum oryx_gcc for BR; enum oryx_fence for GOP_FENCE */
	int      mclass;  /* enum oryx_mclass for LOAD/STORE (default LOCAL) */
};

/*
 * Memory-model lowering policy (Door 3, docs/door3-drf-translation.md):
 *   DRF          — respect each op's mclass: LOCAL weak, SHARED ordered (default)
 *   CONSERVATIVE — order EVERY ordinary access (the Box64 STRONGMEM baseline)
 */
enum oryx_mm_policy { ORYX_POLICY_DRF = 0, ORYX_POLICY_CONSERVATIVE = 1 };

/*
 * How an *ordered* (SHARED) access is lowered — THREE correct choices
 * (docs/box64-fex-integration.md §4). x86-TSO = SC minus the store->load (W->R)
 * relaxation, so the tightest correct mapping keeps R->R/R->W/W->W and relaxes
 * only W->R:
 *   RCPC — LDAPR(RCpc)/STLR: EXACTLY x86-TSO and cheapest (0 extra insns).
 *          Needs FEAT_LRCPC (ARMv8.3; Oryon has it). RCpc differs from RCsc only
 *          by dropping STLR->LDAPR ordering = the W->R/SB case TSO allows; WRC and
 *          IRIW stay forbidden (ARMv8 is other-multi-copy-atomic). This is what
 *          FEX uses for TSO loads. ("RCpc too weak" folklore is about seq_cst,
 *          whose failing test is SB — which TSO permits.)
 *   SC   — LDAR(RCsc)/STLR: STRONGER than TSO (also forbids W->R). Correct but
 *          over-strong; works on ARMv8.0; the safe conservative/diff baseline.
 *          NOTE: LDAR/STLR require natural alignment — a maybe-unaligned SHARED
 *          access must use RCPC/TSO+decompose, not SC, or it will fault.
 *   TSO  — minimal exact-TSO DMB-fence scheme: load = LDR;DMB ISHLD,
 *          store = DMB ISHST;STR. Exact TSO, multi-copy-atomic, works pre-8.3.
 *          (1 DMB per access here; a real translator merges adjacent fences.)
 */
enum oryx_order_strength { ORYX_ORDER_SC = 0, ORYX_ORDER_RCPC = 1, ORYX_ORDER_TSO = 2 };

/* Counts of how each memory op was lowered — the barrier-reduction metric. */
struct oryx_mm_stats {
	uint32_t plain_loads,  plain_stores;    /* weak LDR/STR (free)          */
	uint32_t ordered_loads, ordered_stores; /* LDAR/STLR (the ordering tax) */
	uint32_t atomics;                       /* LSE acq+rel                  */
	uint32_t fences;                        /* DMB                          */
};

/* Identity tags stamped into produced TUs. */
#define ORYX_TU_ISA_SM8850     0x53384850u   /* "SM85" */
#define ORYX_TU_TRANSLATOR_REF 0x4f525958u   /* "ORYX" reference translator */
#define ORYX_TU_TRANSLATOR_VER 1u

/*
 * Translate a basic block. `out` is filled with a fully-owned oryx_tu (free with
 * oryx_tu_free). guest_len is the number of guest bytes the block covers (for
 * bookkeeping/prefetch). profile_id records the tuning/memory-model variant.
 * Returns ORYX_OK or an ORYX_ERR_*.
 *
 * Deterministic and collision-free: the guest hash folds in everything that
 * affects emitted code — each op (including its mclass) plus guest_pc, the
 * memory-model policy, and the ordering strength — so two translations produce
 * the same content address iff they would produce the same host bytes. (This
 * closes a cache-aliasing bug where blocks differing only in mclass/policy/
 * strength shared a key.)
 */
int oryx_translate(const struct oryx_ginsn *ops, size_t n,
		   uint64_t guest_pc, uint32_t guest_len, uint32_t profile_id,
		   struct oryx_tu *out);

/*
 * Door 3 translation with an explicit memory-model policy and optional stats.
 * `oryx_translate` above is exactly this with ORYX_POLICY_DRF and stats = NULL.
 * `stats` (nullable) receives the plain-vs-ordered breakdown.
 */
int oryx_translate_ex(const struct oryx_ginsn *ops, size_t n,
		      uint64_t guest_pc, uint32_t guest_len, uint32_t profile_id,
		      enum oryx_mm_policy policy, enum oryx_order_strength strength,
		      struct oryx_tu *out, struct oryx_mm_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* ORYXTU_H */
