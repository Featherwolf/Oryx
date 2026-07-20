// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryxtu.c — reference deterministic x86-64 -> AArch64 basic-block translator.
 *
 * Real AArch64 instruction encodings, a fixed guest->host register mapping
 * (x86 reg index N -> AArch64 xN, N in 0..15), and relocations for control-flow
 * targets. No host addresses, no time, no RNG -> byte-deterministic output.
 */
#define _GNU_SOURCE
#include "oryxtu.h"

#include <stdlib.h>
#include <string.h>

/* ---- AArch64 encoders (return the 32-bit instruction word) --------------- */
static uint32_t enc_ret(void)              { return 0xD65F03C0u; }                 /* RET x30            */
static uint32_t enc_mov_rr(int rd, int rm) { return 0xAA0003E0u | ((uint32_t)rm<<16) | (uint32_t)rd; } /* ORR Xd,XZR,Xm */
static uint32_t enc_add_rr(int rd,int rn,int rm){ return 0x8B000000u | ((uint32_t)rm<<16) | ((uint32_t)rn<<5) | (uint32_t)rd; }
static uint32_t enc_sub_rr(int rd,int rn,int rm){ return 0xCB000000u | ((uint32_t)rm<<16) | ((uint32_t)rn<<5) | (uint32_t)rd; }
static uint32_t enc_subs_cmp(int rn,int rm){ return 0xEB000000u | ((uint32_t)rm<<16) | ((uint32_t)rn<<5) | 31u; } /* SUBS XZR,Xn,Xm */
static uint32_t enc_movz(int rd,uint32_t imm16,int hw){ return 0xD2800000u | ((uint32_t)hw<<21) | ((imm16&0xffffu)<<5) | (uint32_t)rd; }
static uint32_t enc_movk(int rd,uint32_t imm16,int hw){ return 0xF2800000u | ((uint32_t)hw<<21) | ((imm16&0xffffu)<<5) | (uint32_t)rd; }
static uint32_t enc_ldr(int rt,int rn,uint32_t imm12){ return 0xF9400000u | ((imm12&0xfffu)<<10) | ((uint32_t)rn<<5) | (uint32_t)rt; }
static uint32_t enc_str(int rt,int rn,uint32_t imm12){ return 0xF9000000u | ((imm12&0xfffu)<<10) | ((uint32_t)rn<<5) | (uint32_t)rt; }
static uint32_t enc_b(void)                { return 0x14000000u; }                 /* B +0 (patched)     */
static uint32_t enc_bcond(int cond)        { return 0x54000000u | ((uint32_t)cond & 0xf); } /* B.cond +0 (patched) */
/* --- Door 3: ordered / synchronization forms --- */
static uint32_t enc_ldar(int rt,int rn)    { return 0xC8DFFC00u | ((uint32_t)rn<<5) | (uint32_t)rt; } /* LDAR  Xt,[Xn] (RCsc; LDAPR on FEAT_LRCPC) */
static uint32_t enc_stlr(int rt,int rn)    { return 0xC89FFC00u | ((uint32_t)rn<<5) | (uint32_t)rt; } /* STLR  Xt,[Xn] */
static uint32_t enc_add_imm(int rd,int rn,uint32_t imm12){ return 0x91000000u | ((imm12&0xfffu)<<10) | ((uint32_t)rn<<5) | (uint32_t)rd; } /* ADD Xd,Xn,#imm */
static uint32_t enc_ldaddal(int rs,int rt,int rn){ return 0xF8E00000u | ((uint32_t)rs<<16) | ((uint32_t)rn<<5) | (uint32_t)rt; } /* LDADDAL Xs,Xt,[Xn] */
static uint32_t enc_casal(int rs,int rt,int rn){ return 0xC8E0FC00u | ((uint32_t)rs<<16) | ((uint32_t)rn<<5) | (uint32_t)rt; } /* CASAL Xs,Xt,[Xn] */
static uint32_t enc_dmb(int fence)         { /* DMB ISH / ISHLD / ISHST */
	switch (fence) {
	case ORYX_FENCE_LD: return 0xD50339BFu; /* DMB ISHLD */
	case ORYX_FENCE_ST: return 0xD5033ABFu; /* DMB ISHST */
	default:            return 0xD5033BBFu; /* DMB ISH (full) */
	}
}

/* Scratch register for computing effective addresses of ordered accesses
 * (guest regs map to x0..x15; x16/IP0 is free). */
#define ORYX_SCRATCH 16

/* x86 condition -> AArch64 B.cond condition field. */
static int arm_cond(int gcc)
{
	switch (gcc) {
	case GCC_EQ: return 0x0;  /* EQ */
	case GCC_NE: return 0x1;  /* NE */
	case GCC_GE: return 0xA;  /* GE */
	case GCC_LT: return 0xB;  /* LT */
	case GCC_GT: return 0xC;  /* GT */
	case GCC_LE: return 0xD;  /* LE */
	default:     return 0xE;  /* AL (should not happen) */
	}
}

/* ---- growable buffers ---------------------------------------------------- */
struct buf { uint8_t *p; size_t len, cap; };

static int buf_emit32(struct buf *b, uint32_t insn)
{
	if (b->len + 4 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 64;
		uint8_t *np = realloc(b->p, nc);
		if (!np) return ORYX_ERR_NOMEM;
		b->p = np; b->cap = nc;
	}
	b->p[b->len++] = (uint8_t)(insn);
	b->p[b->len++] = (uint8_t)(insn >> 8);
	b->p[b->len++] = (uint8_t)(insn >> 16);
	b->p[b->len++] = (uint8_t)(insn >> 24);
	return ORYX_OK;
}

/* ---- guest-block hashing (source identity) ------------------------------- */
static void hash_block(const struct oryx_ginsn *ops, size_t n,
		       uint64_t guest_pc, uint8_t out[SHA256_DIGEST_LEN])
{
	/* Canonical little-endian encoding of the IR + entry pc. */
	sha256_ctx c;
	sha256_init(&c);
	uint8_t hdr[8];
	for (int i = 0; i < 8; i++) hdr[i] = (uint8_t)(guest_pc >> (8*i));
	sha256_update(&c, hdr, 8);
	for (size_t i = 0; i < n; i++) {
		uint8_t rec[7*4]; size_t o = 0;
		#define P32(v) do { uint32_t _v=(uint32_t)(v); rec[o++]=_v; rec[o++]=_v>>8; rec[o++]=_v>>16; rec[o++]=_v>>24; } while(0)
		P32(ops[i].op); P32(ops[i].rd); P32(ops[i].rn);
		P32((uint32_t)ops[i].imm); P32((uint32_t)((uint64_t)ops[i].imm>>32));
		P32((uint32_t)ops[i].target); P32(ops[i].cc);
		#undef P32
		sha256_update(&c, rec, o);
	}
	sha256_final(&c, out);
}

/* ---- translate ----------------------------------------------------------- */
int oryx_translate_ex(const struct oryx_ginsn *ops, size_t n,
		      uint64_t guest_pc, uint32_t guest_len, uint32_t profile_id,
		      enum oryx_mm_policy policy, struct oryx_tu *out,
		      struct oryx_mm_stats *stats)
{
	if (!ops || !out)
		return ORYX_ERR_INVAL;

	struct buf code = {0};
	struct oryx_reloc *relocs = NULL; uint32_t nreloc = 0, creloc = 0;
	uint64_t *exits = NULL; uint32_t nexit = 0, cexit = 0;
	int rc = ORYX_OK;
	int terminated = 0;
	struct oryx_mm_stats st = {0};

	#define FAIL(e) do { rc = (e); goto fail; } while (0)
	#define EMIT(insn) do { if (buf_emit32(&code, (insn)) != ORYX_OK) FAIL(ORYX_ERR_NOMEM); } while (0)
	#define ADD_RELOC(off, knd, tgt) do { \
		if (nreloc == creloc) { \
			uint32_t nc = creloc ? creloc*2 : 4; \
			struct oryx_reloc *nr = realloc(relocs, nc*sizeof(*nr)); \
			if (!nr) { FAIL(ORYX_ERR_NOMEM); } \
			relocs = nr; creloc = nc; \
		} \
		relocs[nreloc].offset = (off); relocs[nreloc].kind = (knd); \
		relocs[nreloc].guest_target = (tgt); nreloc++; \
	} while (0)
	#define ADD_EXIT(pc) do { \
		if (nexit == cexit) { \
			uint32_t nc = cexit ? cexit*2 : 4; \
			uint64_t *ne = realloc(exits, nc*sizeof(*ne)); \
			if (!ne) { FAIL(ORYX_ERR_NOMEM); } \
			exits = ne; cexit = nc; \
		} \
		exits[nexit++] = (pc); \
	} while (0)

	for (size_t i = 0; i < n; i++) {
		const struct oryx_ginsn *in = &ops[i];
		if (terminated)
			FAIL(ORYX_ERR_INVAL);          /* code after a block terminator */
		if (in->rd < 0 || in->rd >= GR_COUNT || in->rn < 0 || in->rn >= GR_COUNT)
			FAIL(ORYX_ERR_INVAL);

		switch (in->op) {
		case GOP_MOV_RR:
			EMIT(enc_mov_rr(in->rd, in->rn));
			break;
		case GOP_MOV_RI: {
			uint64_t v = (uint64_t)in->imm;
			EMIT(enc_movz(in->rd, (uint32_t)(v & 0xffff), 0));
			for (int hw = 1; hw < 4; hw++) {
				uint32_t lane = (uint32_t)((v >> (16*hw)) & 0xffff);
				if (lane)
					EMIT(enc_movk(in->rd, lane, hw));
			}
			break;
		}
		case GOP_ADD_RR:
			EMIT(enc_add_rr(in->rd, in->rd, in->rn));
			break;
		case GOP_SUB_RR:
			EMIT(enc_sub_rr(in->rd, in->rd, in->rn));
			break;
		case GOP_CMP_RR:               /* flags from (rd - rn) */
			EMIT(enc_subs_cmp(in->rd, in->rn));
			break;
		case GOP_LOAD:
		case GOP_STORE: {
			/* Door 3: order this access only if it could race.
			 * CONSERVATIVE policy orders everything; DRF respects mclass. */
			int ordered = (policy == ORYX_POLICY_CONSERVATIVE) ||
				      (in->mclass == ORYX_MCLASS_SHARED);
			if (!ordered) {
				/* weak LDR/STR with a built-in scaled offset (cheap path) */
				if (in->imm < 0 || (in->imm & 7) != 0 || (in->imm >> 3) > 0xFFF)
					FAIL(ORYX_ERR_INVAL);  /* need 8-aligned, in range */
				uint32_t imm12 = (uint32_t)(in->imm >> 3);
				if (in->op == GOP_LOAD) { EMIT(enc_ldr(in->rd, in->rn, imm12)); st.plain_loads++; }
				else                    { EMIT(enc_str(in->rd, in->rn, imm12)); st.plain_stores++; }
			} else {
				/* ordered acquire/release: address must be in a register, so
				 * a nonzero displacement costs an extra ADD (imm 0..4095). */
				int base = in->rn;
				if (in->imm < 0 || in->imm > 0xFFF)
					FAIL(ORYX_ERR_INVAL);
				if (in->imm != 0) {
					EMIT(enc_add_imm(ORYX_SCRATCH, in->rn, (uint32_t)in->imm));
					base = ORYX_SCRATCH;
				}
				if (in->op == GOP_LOAD) { EMIT(enc_ldar(in->rd, base)); st.ordered_loads++; }
				else                    { EMIT(enc_stlr(in->rd, base)); st.ordered_stores++; }
			}
			break;
		}
		case GOP_ATOMIC_ADD:               /* LOCK ADD [rn], rd -> LDADDAL (discard old) */
			EMIT(enc_ldaddal(in->rd, 31 /*XZR*/, in->rn));
			st.atomics++;
			break;
		case GOP_ATOMIC_CAS:               /* LOCK CMPXCHG: RAX=compare, rd=desired */
			EMIT(enc_casal(GR_RAX, in->rd, in->rn));
			st.atomics++;
			break;
		case GOP_FENCE:                    /* MFENCE/LFENCE/SFENCE -> DMB */
			EMIT(enc_dmb(in->cc));
			st.fences++;
			break;
		case GOP_BR:
			ADD_RELOC((uint32_t)code.len, ORYX_RELOC_BRANCH_GUEST_PC, in->target);
			EMIT(enc_bcond(arm_cond(in->cc)));
			ADD_EXIT(in->target);
			ADD_EXIT(guest_pc + guest_len);   /* fallthrough */
			terminated = 1;
			break;
		case GOP_JMP:
			ADD_RELOC((uint32_t)code.len, ORYX_RELOC_BRANCH_GUEST_PC, in->target);
			EMIT(enc_b());
			ADD_EXIT(in->target);
			terminated = 1;
			break;
		case GOP_RET:
			EMIT(enc_ret());
			terminated = 1;
			break;
		default:
			FAIL(ORYX_ERR_INVAL);
		}
	}

	/* A well-formed block must end in a terminator. */
	if (!terminated)
		FAIL(ORYX_ERR_INVAL);

	memset(out, 0, sizeof(*out));
	out->isa_id = ORYX_TU_ISA_SM8850;
	out->translator_id = ORYX_TU_TRANSLATOR_REF;
	out->translator_version = ORYX_TU_TRANSLATOR_VER;
	out->profile_id = profile_id;
	out->flags = 0;
	out->guest_entry_pc = guest_pc;
	out->guest_len = guest_len;
	hash_block(ops, n, guest_pc, out->guest_hash);
	out->code = code.p;
	out->code_len = (uint32_t)code.len;
	out->relocs = relocs;
	out->reloc_count = nreloc;
	out->exits = exits;
	out->exit_count = nexit;
	if (stats)
		*stats = st;
	return ORYX_OK;

fail:
	free(code.p);
	free(relocs);
	free(exits);
	return rc;
}

/* Convenience: the default translator is DRF policy with no stats reporting. */
int oryx_translate(const struct oryx_ginsn *ops, size_t n,
		   uint64_t guest_pc, uint32_t guest_len, uint32_t profile_id,
		   struct oryx_tu *out)
{
	return oryx_translate_ex(ops, n, guest_pc, guest_len, profile_id,
				 ORYX_POLICY_DRF, out, NULL);
}
