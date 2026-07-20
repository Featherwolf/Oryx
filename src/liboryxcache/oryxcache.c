// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryxcache.c — reference implementation of the Oryx Part B content-addressed
 * translation/shader cache. Fail-closed integrity throughout.
 */
#define _GNU_SOURCE
#include "oryxcache.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---- little-endian encode/decode ---------------------------------------- */
static void put_u16(uint8_t *p, uint16_t v){ p[0]=v; p[1]=v>>8; }
static void put_u32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put_u64(uint8_t *p, uint64_t v){ for(int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint16_t get_u16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t get_u32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
	((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t get_u64(const uint8_t *p){ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v; }

#define TU_HEADER_LEN 84
#define TU_RELOC_LEN  16
#define TU_EXIT_LEN   8

/* ---- TU serialize -------------------------------------------------------- */
int oryx_tu_serialize(const struct oryx_tu *tu, uint8_t **out, size_t *out_len)
{
	if (!tu || !out || !out_len)
		return ORYX_ERR_INVAL;

	size_t total = TU_HEADER_LEN
		+ (size_t)tu->code_len
		+ (size_t)tu->reloc_count * TU_RELOC_LEN
		+ (size_t)tu->exit_count * TU_EXIT_LEN;

	uint8_t *b = malloc(total);
	if (!b)
		return ORYX_ERR_NOMEM;

	put_u32(b + 0,  ORYX_TU_MAGIC);
	put_u16(b + 4,  ORYX_TU_VERSION);
	put_u16(b + 6,  0);
	put_u32(b + 8,  tu->isa_id);
	put_u32(b + 12, tu->translator_id);
	put_u32(b + 16, tu->translator_version);
	put_u32(b + 20, tu->profile_id);
	put_u32(b + 24, tu->flags);
	put_u64(b + 28, tu->guest_entry_pc);
	put_u32(b + 36, tu->guest_len);
	put_u32(b + 40, tu->code_len);
	put_u32(b + 44, tu->reloc_count);
	put_u32(b + 48, tu->exit_count);
	memcpy(b + 52, tu->guest_hash, SHA256_DIGEST_LEN);

	uint8_t *p = b + TU_HEADER_LEN;
	if (tu->code_len) { memcpy(p, tu->code, tu->code_len); p += tu->code_len; }
	for (uint32_t i = 0; i < tu->reloc_count; i++) {
		put_u32(p + 0, tu->relocs[i].offset);
		put_u32(p + 4, tu->relocs[i].kind);
		put_u64(p + 8, tu->relocs[i].guest_target);
		p += TU_RELOC_LEN;
	}
	for (uint32_t i = 0; i < tu->exit_count; i++) {
		put_u64(p, tu->exits[i]);
		p += TU_EXIT_LEN;
	}

	*out = b;
	*out_len = total;
	return ORYX_OK;
}

/* ---- TU parse ------------------------------------------------------------ */
int oryx_tu_parse(const uint8_t *buf, size_t len, struct oryx_tu *out)
{
	if (!buf || !out || len < TU_HEADER_LEN)
		return ORYX_ERR_FORMAT;
	if (get_u32(buf + 0) != ORYX_TU_MAGIC)
		return ORYX_ERR_FORMAT;
	if (get_u16(buf + 4) != ORYX_TU_VERSION)
		return ORYX_ERR_FORMAT;

	memset(out, 0, sizeof(*out));
	out->isa_id             = get_u32(buf + 8);
	out->translator_id      = get_u32(buf + 12);
	out->translator_version = get_u32(buf + 16);
	out->profile_id         = get_u32(buf + 20);
	out->flags              = get_u32(buf + 24);
	out->guest_entry_pc     = get_u64(buf + 28);
	out->guest_len          = get_u32(buf + 36);
	out->code_len           = get_u32(buf + 40);
	out->reloc_count        = get_u32(buf + 44);
	out->exit_count         = get_u32(buf + 48);
	memcpy(out->guest_hash, buf + 52, SHA256_DIGEST_LEN);

	/* Validate that all sections fit EXACTLY (canonical form -> determinism). */
	size_t need = TU_HEADER_LEN
		+ (size_t)out->code_len
		+ (size_t)out->reloc_count * TU_RELOC_LEN
		+ (size_t)out->exit_count * TU_EXIT_LEN;
	if (need != len)
		return ORYX_ERR_FORMAT;

	const uint8_t *p = buf + TU_HEADER_LEN;
	if (out->code_len) {
		out->code = malloc(out->code_len);
		if (!out->code) goto nomem;
		memcpy(out->code, p, out->code_len);
		p += out->code_len;
	}
	if (out->reloc_count) {
		out->relocs = calloc(out->reloc_count, sizeof(*out->relocs));
		if (!out->relocs) goto nomem;
		for (uint32_t i = 0; i < out->reloc_count; i++) {
			out->relocs[i].offset       = get_u32(p + 0);
			out->relocs[i].kind         = get_u32(p + 4);
			out->relocs[i].guest_target = get_u64(p + 8);
			p += TU_RELOC_LEN;
		}
	}
	if (out->exit_count) {
		out->exits = calloc(out->exit_count, sizeof(*out->exits));
		if (!out->exits) goto nomem;
		for (uint32_t i = 0; i < out->exit_count; i++) {
			out->exits[i] = get_u64(p);
			p += TU_EXIT_LEN;
		}
	}
	return ORYX_OK;

nomem:
	oryx_tu_free(out);
	return ORYX_ERR_NOMEM;
}

void oryx_tu_free(struct oryx_tu *tu)
{
	if (!tu)
		return;
	free(tu->code);   tu->code = NULL;
	free(tu->relocs); tu->relocs = NULL;
	free(tu->exits);  tu->exits = NULL;
}

/* ---- keys / addresses ---------------------------------------------------- */
void oryx_content_address(const void *buf, size_t len, char hex[SHA256_HEX_LEN + 1])
{
	uint8_t d[SHA256_DIGEST_LEN];
	sha256(buf, len, d);
	sha256_hex(d, hex);
}

void oryx_tu_logical_key(const struct oryx_tu *tu, char hex[SHA256_HEX_LEN + 1])
{
	/* Canonical key preimage: guest_hash ‖ translator_id ‖ translator_version
	 * ‖ profile_id ‖ isa_id, all little-endian. */
	uint8_t pre[SHA256_DIGEST_LEN + 16];
	memcpy(pre, tu->guest_hash, SHA256_DIGEST_LEN);
	put_u32(pre + 32, tu->translator_id);
	put_u32(pre + 36, tu->translator_version);
	put_u32(pre + 40, tu->profile_id);
	put_u32(pre + 44, tu->isa_id);

	uint8_t d[SHA256_DIGEST_LEN];
	sha256(pre, sizeof(pre), d);
	sha256_hex(d, hex);
}

/* ---- file helpers -------------------------------------------------------- */
static int mkdir_p(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return ORYX_OK;
	return ORYX_ERR_IO;
}

static int is_hex64(const char *s)
{
	if (!s) return 0;
	for (int i = 0; i < SHA256_HEX_LEN; i++) {
		char ch = s[i];
		if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
			return 0;
	}
	return s[SHA256_HEX_LEN] == '\0';
}

/* Atomic write: tmp file (unique by pid) -> fsync -> rename. */
static int write_atomic(const char *final_path, const void *data, size_t len)
{
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", final_path, (long)getpid());

	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return ORYX_ERR_IO;

	const uint8_t *p = data;
	size_t left = len;
	while (left > 0) {
		ssize_t n = write(fd, p, left);
		if (n < 0) { close(fd); unlink(tmp); return ORYX_ERR_IO; }
		p += n; left -= (size_t)n;
	}
	if (fsync(fd) != 0) { close(fd); unlink(tmp); return ORYX_ERR_IO; }
	close(fd);
	if (rename(tmp, final_path) != 0) { unlink(tmp); return ORYX_ERR_IO; }
	return ORYX_OK;
}

static int read_file(const char *path, uint8_t **out, size_t *out_len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return (errno == ENOENT) ? ORYX_ERR_NOTFOUND : ORYX_ERR_IO;

	struct stat st;
	if (fstat(fd, &st) != 0) { close(fd); return ORYX_ERR_IO; }

	size_t len = (size_t)st.st_size;
	uint8_t *buf = malloc(len ? len : 1);
	if (!buf) { close(fd); return ORYX_ERR_NOMEM; }

	size_t got = 0;
	while (got < len) {
		ssize_t n = read(fd, buf + got, len - got);
		if (n < 0) { free(buf); close(fd); return ORYX_ERR_IO; }
		if (n == 0) break;
		got += (size_t)n;
	}
	close(fd);
	*out = buf;
	*out_len = got;
	return ORYX_OK;
}

/* ---- cache store --------------------------------------------------------- */
int oryx_cache_open(struct oryx_cache *c, const char *root)
{
	char sub[600];
	if (!c || !root || strlen(root) >= sizeof(c->root))
		return ORYX_ERR_INVAL;

	snprintf(c->root, sizeof(c->root), "%s", root);
	if (mkdir_p(c->root) != ORYX_OK)
		return ORYX_ERR_IO;
	snprintf(sub, sizeof(sub), "%s/blobs", c->root);
	if (mkdir_p(sub) != ORYX_OK)
		return ORYX_ERR_IO;
	snprintf(sub, sizeof(sub), "%s/index", c->root);
	if (mkdir_p(sub) != ORYX_OK)
		return ORYX_ERR_IO;
	return ORYX_OK;
}

int oryx_cache_put_blob(struct oryx_cache *c, const void *data, size_t len,
			const char *logical_key_hex, char addr_out[SHA256_HEX_LEN + 1])
{
	if (!c || !data || !logical_key_hex || !addr_out)
		return ORYX_ERR_INVAL;
	if (!is_hex64(logical_key_hex))
		return ORYX_ERR_INVAL;

	char addr[SHA256_HEX_LEN + 1];
	oryx_content_address(data, len, addr);

	char blob_path[700];
	snprintf(blob_path, sizeof(blob_path), "%s/blobs/%s", c->root, addr);

	/* Only write the blob if absent (content-addressed => identical bytes). */
	if (access(blob_path, F_OK) != 0) {
		int rc = write_atomic(blob_path, data, len);
		if (rc != ORYX_OK)
			return rc;
	}

	/* Point the index entry at the content address. */
	char idx_path[700];
	snprintf(idx_path, sizeof(idx_path), "%s/index/%s", c->root, logical_key_hex);
	int rc = write_atomic(idx_path, addr, SHA256_HEX_LEN);
	if (rc != ORYX_OK)
		return rc;

	memcpy(addr_out, addr, SHA256_HEX_LEN + 1);
	return ORYX_OK;
}

int oryx_cache_get_blob(struct oryx_cache *c, const char *addr_hex,
			uint8_t **out, size_t *out_len)
{
	if (!c || !is_hex64(addr_hex) || !out || !out_len)
		return ORYX_ERR_INVAL;

	char blob_path[700];
	snprintf(blob_path, sizeof(blob_path), "%s/blobs/%s", c->root, addr_hex);

	uint8_t *buf; size_t len;
	int rc = read_file(blob_path, &buf, &len);
	if (rc != ORYX_OK)
		return rc;

	/* Fail-closed integrity: the bytes must hash to the address we asked for. */
	char actual[SHA256_HEX_LEN + 1];
	oryx_content_address(buf, len, actual);
	if (memcmp(actual, addr_hex, SHA256_HEX_LEN) != 0) {
		free(buf);
		return ORYX_ERR_INTEGRITY;
	}

	*out = buf;
	*out_len = len;
	return ORYX_OK;
}

int oryx_cache_resolve(struct oryx_cache *c, const char *logical_key_hex,
		       char addr_out[SHA256_HEX_LEN + 1])
{
	if (!c || !is_hex64(logical_key_hex) || !addr_out)
		return ORYX_ERR_INVAL;

	char idx_path[700];
	snprintf(idx_path, sizeof(idx_path), "%s/index/%s", c->root, logical_key_hex);

	uint8_t *buf; size_t len;
	int rc = read_file(idx_path, &buf, &len);
	if (rc != ORYX_OK)
		return rc;
	if (len < SHA256_HEX_LEN) { free(buf); return ORYX_ERR_FORMAT; }

	memcpy(addr_out, buf, SHA256_HEX_LEN);
	addr_out[SHA256_HEX_LEN] = '\0';
	free(buf);
	if (!is_hex64(addr_out))
		return ORYX_ERR_FORMAT;
	return ORYX_OK;
}

int oryx_cache_put_tu(struct oryx_cache *c, const struct oryx_tu *tu,
		      char addr_out[SHA256_HEX_LEN + 1])
{
	uint8_t *bytes; size_t len;
	int rc = oryx_tu_serialize(tu, &bytes, &len);
	if (rc != ORYX_OK)
		return rc;

	char key[SHA256_HEX_LEN + 1];
	oryx_tu_logical_key(tu, key);

	rc = oryx_cache_put_blob(c, bytes, len, key, addr_out);
	free(bytes);
	return rc;
}

int oryx_cache_get_tu(struct oryx_cache *c, const char *logical_key_hex,
		      struct oryx_tu *out)
{
	char addr[SHA256_HEX_LEN + 1];
	int rc = oryx_cache_resolve(c, logical_key_hex, addr);
	if (rc != ORYX_OK)
		return rc;

	uint8_t *bytes; size_t len;
	rc = oryx_cache_get_blob(c, addr, &bytes, &len);
	if (rc != ORYX_OK)
		return rc;

	rc = oryx_tu_parse(bytes, len, out);
	free(bytes);
	return rc;
}

int oryx_cache_verify(struct oryx_cache *c, const char *addr_hex)
{
	uint8_t *buf; size_t len;
	int rc = oryx_cache_get_blob(c, addr_hex, &buf, &len); /* re-hashes internally */
	if (rc == ORYX_OK)
		free(buf);
	return rc;
}
