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
	GOP_CMP_RR,      /* flags = rn - rd (sets NZCV)      */
	GOP_LOAD,        /* rd = [rn + imm]  (imm: 0..32760, 8-aligned) */
	GOP_STORE,       /* [rn + imm] = rd                  */
	GOP_BR,          /* if (cc) goto target ; terminates */
	GOP_JMP,         /* goto target      ; terminates    */
	GOP_RET          /* return           ; terminates    */
};

struct oryx_ginsn {
	int      op;      /* enum oryx_gop */
	int      rd;      /* enum oryx_greg */
	int      rn;      /* enum oryx_greg */
	int64_t  imm;     /* MOV_RI value, or LOAD/STORE displacement */
	uint64_t target;  /* guest PC for BR/JMP */
	int      cc;      /* enum oryx_gcc for BR */
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
 * Deterministic: for identical (ops, n, guest_pc, guest_len, profile_id) the
 * serialized TU — and therefore its content address — is byte-identical.
 */
int oryx_translate(const struct oryx_ginsn *ops, size_t n,
		   uint64_t guest_pc, uint32_t guest_len, uint32_t profile_id,
		   struct oryx_tu *out);

#ifdef __cplusplus
}
#endif

#endif /* ORYXTU_H */
