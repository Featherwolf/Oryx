// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryxdec.c — x86-64 integer decoder. Decodes one basic block of real machine
 * code into liboryxtu guest IR. See oryxdec.h for the supported subset.
 */
#include "oryxdec.h"

/* ---- little-endian reads (all bounds-checked by the caller) -------------- */
static uint32_t rd_u32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t rd_u64(const uint8_t *p)
{ uint64_t v=0; for (int i=0;i<8;i++) v |= (uint64_t)p[i] << (8*i); return v; }

/* Decoded ModRM. For register-direct (mod==3) rm_reg is the operand register;
 * for a memory operand, base + disp (SIB / RIP-relative are unsupported here). */
struct modrm {
	int      reg;      /* ModRM.reg (+REX.R) — a register or an opcode-group digit */
	int      is_mem;
	int      rm_reg;   /* register-direct operand (+REX.B) */
	int      base;     /* memory base register (+REX.B) */
	int64_t  disp;
};

/* Decode a ModRM (and any displacement) at *off; advance *off and *pc. */
static int decode_modrm(const struct oryx_x86_image *img, size_t *off, uint64_t *pc,
			int rexR, int rexB, struct modrm *m)
{
	if (*off + 1 > img->len)
		return ORYX_ERR_FORMAT;
	uint8_t b = img->bytes[*off]; (*off)++; (*pc)++;
	int mod = b >> 6, reg = (b >> 3) & 7, rm = b & 7;
	m->reg = reg | (rexR << 3);
	m->disp = 0;

	if (mod == 3) {                       /* register-direct */
		m->is_mem = 0;
		m->rm_reg = rm | (rexB << 3);
		return ORYX_OK;
	}
	m->is_mem = 1;
	if (rm == 4)                          /* SIB — not in the v1 subset */
		return ORYX_ERR_UNSUPPORTED;
	if (rm == 5 && mod == 0)              /* RIP-relative — not in the v1 subset */
		return ORYX_ERR_UNSUPPORTED;

	m->base = rm | (rexB << 3);
	if (mod == 1) {                       /* disp8 (sign-extended) */
		if (*off + 1 > img->len) return ORYX_ERR_FORMAT;
		m->disp = (int8_t)img->bytes[*off]; (*off)++; (*pc)++;
	} else if (mod == 2) {                /* disp32 (sign-extended) */
		if (*off + 4 > img->len) return ORYX_ERR_FORMAT;
		m->disp = (int32_t)rd_u32(&img->bytes[*off]); (*off) += 4; (*pc) += 4;
	}                                     /* mod == 0 -> disp 0 */
	return ORYX_OK;
}

/* Map the low nibble of a Jcc opcode (the x86 `tttn`) to an IR condition.
 * Only the signed + equality conditions are in the v1 subset. */
static int x86cc_to_gcc(int cc, int *out)
{
	switch (cc) {
	case 0x4: *out = GCC_EQ; return 1;    /* JE / JZ   */
	case 0x5: *out = GCC_NE; return 1;    /* JNE / JNZ */
	case 0xC: *out = GCC_LT; return 1;    /* JL        */
	case 0xD: *out = GCC_GE; return 1;    /* JGE       */
	case 0xE: *out = GCC_LE; return 1;    /* JLE       */
	case 0xF: *out = GCC_GT; return 1;    /* JG        */
	default:  return 0;                   /* unsigned / flag conditions: unsupported */
	}
}

#define EMIT(ginsn) do { \
	if (n >= cap) return ORYX_ERR_INVAL; \
	ops[n++] = (ginsn); \
} while (0)

int oryx_x86_decode_block(const struct oryx_x86_image *img, uint64_t pc,
			  struct oryx_ginsn *ops, size_t cap,
			  size_t *n_out, uint32_t *len_out)
{
	if (!img || !img->bytes || !ops || !n_out || !len_out)
		return ORYX_ERR_INVAL;
	if (pc < img->base || pc >= img->base + img->len)
		return ORYX_ERR_NOTFOUND;

	size_t   off  = pc - img->base;
	uint64_t cur  = pc;
	size_t   n    = 0;
	int      done = 0;

	while (!done) {
		if (off + 1 > img->len)
			return ORYX_ERR_FORMAT;

		/* endbr64 = F3 0F 1E FA -> no-op */
		if (img->bytes[off] == 0xF3 && off + 4 <= img->len &&
		    img->bytes[off+1] == 0x0F && img->bytes[off+2] == 0x1E &&
		    img->bytes[off+3] == 0xFA) {
			off += 4; cur += 4;
			continue;
		}

		/* REX prefix */
		int rexW = 0, rexR = 0, rexX = 0, rexB = 0;
		uint8_t b = img->bytes[off];
		if (b >= 0x40 && b <= 0x4F) {
			rexW = (b>>3)&1; rexR = (b>>2)&1; rexX = (b>>1)&1; rexB = b&1;
			(void)rexX;
			off++; cur++;
			if (off + 1 > img->len) return ORYX_ERR_FORMAT;
			b = img->bytes[off];
		}
		uint8_t op = b; off++; cur++;
		struct modrm mrm;
		int rc, gcc;

		switch (op) {
		/* ---- MOV ---- */
		case 0x89:  /* MOV r/m, r */
			if ((rc = decode_modrm(img, &off, &cur, rexR, rexB, &mrm)) != ORYX_OK) return rc;
			if (mrm.is_mem) return ORYX_ERR_UNSUPPORTED;
			EMIT(((struct oryx_ginsn){ .op = GOP_MOV_RR, .rd = mrm.rm_reg, .rn = mrm.reg }));
			break;
		case 0x8B:  /* MOV r, r/m */
			if ((rc = decode_modrm(img, &off, &cur, rexR, rexB, &mrm)) != ORYX_OK) return rc;
			if (mrm.is_mem) return ORYX_ERR_UNSUPPORTED;
			EMIT(((struct oryx_ginsn){ .op = GOP_MOV_RR, .rd = mrm.reg, .rn = mrm.rm_reg }));
			break;
		case 0x8D:  /* LEA r, m */
			if ((rc = decode_modrm(img, &off, &cur, rexR, rexB, &mrm)) != ORYX_OK) return rc;
			if (!mrm.is_mem) return ORYX_ERR_UNSUPPORTED;   /* LEA needs a memory operand */
			EMIT(((struct oryx_ginsn){ .op = GOP_LEA, .rd = mrm.reg, .rn = mrm.base, .imm = mrm.disp }));
			break;
		case 0xC7: { /* MOV r/m, imm32 (sign-extended) — /0 only */
			if ((rc = decode_modrm(img, &off, &cur, rexR, rexB, &mrm)) != ORYX_OK) return rc;
			if (mrm.is_mem || (mrm.reg & 7) != 0) return ORYX_ERR_UNSUPPORTED;
			if (off + 4 > img->len) return ORYX_ERR_FORMAT;
			int64_t imm = (int32_t)rd_u32(&img->bytes[off]); off += 4; cur += 4;
			EMIT(((struct oryx_ginsn){ .op = GOP_MOV_RI, .rd = mrm.rm_reg, .imm = imm }));
			break;
		}

		/* ---- ALU reg,reg (r/m,r) ---- */
		case 0x01:  /* ADD r/m, r */
		case 0x29:  /* SUB r/m, r */
		case 0x31:  /* XOR r/m, r */
		case 0x21:  /* AND r/m, r */
		case 0x09:  /* OR  r/m, r */
		case 0x39:  /* CMP r/m, r */
		case 0x85:  /* TEST r/m, r */
			if ((rc = decode_modrm(img, &off, &cur, rexR, rexB, &mrm)) != ORYX_OK) return rc;
			if (mrm.is_mem) return ORYX_ERR_UNSUPPORTED;
			{
				int gop = (op==0x01)?GOP_ADD_RR : (op==0x29)?GOP_SUB_RR :
					  (op==0x31)?GOP_XOR_RR : (op==0x21)?GOP_AND_RR :
					  (op==0x09)?GOP_OR_RR  : (op==0x39)?GOP_CMP_RR : GOP_TEST_RR;
				EMIT(((struct oryx_ginsn){ .op = gop, .rd = mrm.rm_reg, .rn = mrm.reg }));
			}
			break;
		case 0x3B:  /* CMP r, r/m */
			if ((rc = decode_modrm(img, &off, &cur, rexR, rexB, &mrm)) != ORYX_OK) return rc;
			if (mrm.is_mem) return ORYX_ERR_UNSUPPORTED;
			EMIT(((struct oryx_ginsn){ .op = GOP_CMP_RR, .rd = mrm.reg, .rn = mrm.rm_reg }));
			break;

		/* ---- ALU r/m, imm (group 1) ---- */
		case 0x83:  /* r/m, imm8 (sign-extended) */
		case 0x81:  /* r/m, imm32 (sign-extended) */
			if ((rc = decode_modrm(img, &off, &cur, rexR, rexB, &mrm)) != ORYX_OK) return rc;
			if (mrm.is_mem) return ORYX_ERR_UNSUPPORTED;
			{
				int64_t imm;
				if (op == 0x83) {
					if (off + 1 > img->len) return ORYX_ERR_FORMAT;
					imm = (int8_t)img->bytes[off]; off += 1; cur += 1;
				} else {
					if (off + 4 > img->len) return ORYX_ERR_FORMAT;
					imm = (int32_t)rd_u32(&img->bytes[off]); off += 4; cur += 4;
				}
				int digit = mrm.reg & 7;      /* the /digit selects the operation */
				int gop;
				if      (digit == 0) gop = GOP_ADD_RI;   /* ADD */
				else if (digit == 5) gop = GOP_SUB_RI;   /* SUB */
				else if (digit == 7) gop = GOP_CMP_RI;   /* CMP */
				else return ORYX_ERR_UNSUPPORTED;        /* OR/AND/XOR-imm: later */
				EMIT(((struct oryx_ginsn){ .op = gop, .rd = mrm.rm_reg, .imm = imm }));
			}
			break;

		/* ---- MOV r, imm (B8+r) ---- */
		case 0xB8: case 0xB9: case 0xBA: case 0xBB:
		case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
			int reg = (op - 0xB8) | (rexB << 3);
			int64_t imm;
			if (rexW) {                       /* mov r64, imm64 */
				if (off + 8 > img->len) return ORYX_ERR_FORMAT;
				imm = (int64_t)rd_u64(&img->bytes[off]); off += 8; cur += 8;
			} else {                          /* mov r32, imm32 (zero-extended) */
				if (off + 4 > img->len) return ORYX_ERR_FORMAT;
				imm = (int64_t)(uint64_t)rd_u32(&img->bytes[off]); off += 4; cur += 4;
			}
			EMIT(((struct oryx_ginsn){ .op = GOP_MOV_RI, .rd = reg, .imm = imm }));
			break;
		}

		/* ---- control flow (terminators) ---- */
		case 0x70: case 0x71: case 0x72: case 0x73:
		case 0x74: case 0x75: case 0x76: case 0x77:
		case 0x78: case 0x79: case 0x7A: case 0x7B:
		case 0x7C: case 0x7D: case 0x7E: case 0x7F: { /* Jcc rel8 */
			if (off + 1 > img->len) return ORYX_ERR_FORMAT;
			int64_t rel = (int8_t)img->bytes[off]; off += 1; cur += 1;
			if (!x86cc_to_gcc(op & 0xf, &gcc)) return ORYX_ERR_UNSUPPORTED;
			EMIT(((struct oryx_ginsn){ .op = GOP_BR, .cc = gcc, .target = cur + (uint64_t)rel }));
			done = 1;
			break;
		}
		case 0x0F: { /* two-byte: Jcc rel32 (0F 80..8F) */
			if (off + 1 > img->len) return ORYX_ERR_FORMAT;
			uint8_t op2 = img->bytes[off]; off += 1; cur += 1;
			if (op2 >= 0x80 && op2 <= 0x8F) {
				if (off + 4 > img->len) return ORYX_ERR_FORMAT;
				int64_t rel = (int32_t)rd_u32(&img->bytes[off]); off += 4; cur += 4;
				if (!x86cc_to_gcc(op2 & 0xf, &gcc)) return ORYX_ERR_UNSUPPORTED;
				EMIT(((struct oryx_ginsn){ .op = GOP_BR, .cc = gcc, .target = cur + (uint64_t)rel }));
				done = 1;
			} else {
				return ORYX_ERR_UNSUPPORTED;
			}
			break;
		}
		case 0xEB: { /* JMP rel8 */
			if (off + 1 > img->len) return ORYX_ERR_FORMAT;
			int64_t rel = (int8_t)img->bytes[off]; off += 1; cur += 1;
			EMIT(((struct oryx_ginsn){ .op = GOP_JMP, .target = cur + (uint64_t)rel }));
			done = 1;
			break;
		}
		case 0xE9: { /* JMP rel32 */
			if (off + 4 > img->len) return ORYX_ERR_FORMAT;
			int64_t rel = (int32_t)rd_u32(&img->bytes[off]); off += 4; cur += 4;
			EMIT(((struct oryx_ginsn){ .op = GOP_JMP, .target = cur + (uint64_t)rel }));
			done = 1;
			break;
		}
		case 0xC3: /* RET */
			EMIT(((struct oryx_ginsn){ .op = GOP_RET }));
			done = 1;
			break;

		default:
			return ORYX_ERR_UNSUPPORTED;
		}
	}

	*n_out = n;
	*len_out = (uint32_t)(cur - pc);
	return ORYX_OK;
}

int oryx_x86_fetch(uint64_t pc, struct oryx_ginsn *ops, size_t cap,
		   size_t *n, uint32_t *len, void *ctx)
{
	return oryx_x86_decode_block((const struct oryx_x86_image *)ctx, pc, ops, cap, n, len);
}
