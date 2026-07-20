/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * oryxdec.h — a real x86-64 machine-code decoder (integer subset).
 *
 * Turns actual compiled x86-64 bytes into the liboryxtu guest IR, one basic
 * block at a time, so the translate->link->execute pipeline can run programs
 * emitted by a real compiler rather than hand-built IR. It plugs straight into
 * the liboryxrun dispatcher via oryx_x86_fetch.
 *
 * Scope (v1): the register-integer subset a compiled leaf function uses — REX
 * prefixes, ModRM (register-direct + [base+disp] memory for LEA), MOV/ADD/SUB/
 * CMP/TEST/XOR/AND/OR (reg,reg and reg,imm forms), LEA, the signed Jcc set, JMP,
 * RET, and endbr64 (decoded as a no-op). Verified by decoding real gcc -O1
 * output and executing it. NOT yet handled (returns ORYX_ERR_UNSUPPORTED, a
 * clean boundary): memory-operand data instructions (load/store/RMW), SIB and
 * RIP-relative addressing, PUSH/POP/CALL, the unsigned/flag Jcc conditions,
 * operand-size (0x66) and 8/16-bit forms, and the SSE/AVX maps.
 */
#ifndef ORYXDEC_H
#define ORYXDEC_H

#include <stddef.h>
#include <stdint.h>
#include "oryxcache.h"   /* error codes */
#include "oryxtu.h"      /* struct oryx_ginsn (decoder output) */

#ifdef __cplusplus
extern "C" {
#endif

/* Shared with liboryxrun (same guarded value); an opcode/form outside the v1
 * subset decodes to this rather than a hard failure. */
#ifndef ORYX_ERR_UNSUPPORTED
#define ORYX_ERR_UNSUPPORTED (-100)
#endif

/* A flat guest code image: `bytes[0]` lives at guest address `base`. */
struct oryx_x86_image {
	const uint8_t *bytes;
	size_t         len;
	uint64_t       base;
};

/*
 * Decode the basic block at guest PC `pc` into `ops` (capacity `cap`). Stops
 * after the first terminator (Jcc/JMP/RET). Sets `*n_out` (op count) and
 * `*len_out` (guest bytes the block spans, so the fall-through PC is pc+*len_out).
 * Returns ORYX_OK; ORYX_ERR_NOTFOUND if pc is outside the image; ORYX_ERR_FORMAT
 * if the stream is truncated; ORYX_ERR_UNSUPPORTED on an opcode/form outside the
 * v1 subset; ORYX_ERR_INVAL if the block needs more than `cap` ops.
 */
int oryx_x86_decode_block(const struct oryx_x86_image *img, uint64_t pc,
			  struct oryx_ginsn *ops, size_t cap,
			  size_t *n_out, uint32_t *len_out);

/*
 * oryx_fetch_fn adapter for oryx_run_program: `ctx` must point to a
 * struct oryx_x86_image. Lets the dispatcher run a decoded x86-64 program.
 */
int oryx_x86_fetch(uint64_t pc, struct oryx_ginsn *ops, size_t cap,
		   size_t *n, uint32_t *len, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ORYXDEC_H */
