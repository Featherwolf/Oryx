// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * test_oryxcache.c — unit tests for liboryxcache. Exercises the format,
 * determinism, the content-addressed store, and (critically) fail-closed
 * integrity: a tampered blob must be rejected before use.
 */
#define _GNU_SOURCE
#include "oryxcache.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int g_pass, g_fail;
#define CHECK(cond, msg) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

#define TESTDIR "./_testcache"

static void rm_rf(const char *dir)
{
	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd) != 0) { /* ignore */ }
}

/* Build a representative TU with code, relocs, and exits. */
static void make_tu(struct oryx_tu *tu)
{
	static uint8_t code[12] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b};
	static struct oryx_reloc relocs[2] = {
		{ .offset = 4, .kind = ORYX_RELOC_BRANCH_GUEST_PC, .guest_target = 0x401000 },
		{ .offset = 8, .kind = ORYX_RELOC_ABS64_GUEST_PC,  .guest_target = 0x402000 },
	};
	static uint64_t exits[2] = { 0x401000, 0x402000 };

	memset(tu, 0, sizeof(*tu));
	tu->isa_id = 0x53384850;            /* "SM85"-ish tag */
	tu->translator_id = 1;              /* box64 */
	tu->translator_version = 0x00030200;
	tu->profile_id = 7;
	tu->flags = 0;
	tu->guest_entry_pc = 0x400000;
	tu->guest_len = 16;
	sha256("guest-block-bytes-stand-in", 26, tu->guest_hash);
	tu->code = code; tu->code_len = sizeof(code);
	tu->relocs = relocs; tu->reloc_count = 2;
	tu->exits = exits; tu->exit_count = 2;
}

static void test_sha256_vector(void)
{
	printf("test: sha256 known-answer\n");
	uint8_t d[32]; char hex[65];
	sha256("abc", 3, d);
	sha256_hex(d, hex);
	/* FIPS 180-4 example */
	CHECK(strcmp(hex,
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
		"sha256(\"abc\")");
}

static void test_roundtrip_and_determinism(void)
{
	printf("test: TU serialize/parse round-trip + determinism\n");
	struct oryx_tu tu; make_tu(&tu);

	uint8_t *b1, *b2; size_t l1, l2;
	CHECK(oryx_tu_serialize(&tu, &b1, &l1) == ORYX_OK, "serialize #1");
	CHECK(oryx_tu_serialize(&tu, &b2, &l2) == ORYX_OK, "serialize #2");
	CHECK(l1 == l2 && memcmp(b1, b2, l1) == 0, "serialization is deterministic");

	char a1[65], a2[65];
	oryx_content_address(b1, l1, a1);
	oryx_content_address(b2, l2, a2);
	CHECK(strcmp(a1, a2) == 0, "identical inputs -> identical content address");

	struct oryx_tu got;
	CHECK(oryx_tu_parse(b1, l1, &got) == ORYX_OK, "parse");
	CHECK(got.isa_id == tu.isa_id, "isa_id preserved");
	CHECK(got.guest_entry_pc == tu.guest_entry_pc, "guest_entry_pc preserved");
	CHECK(got.code_len == tu.code_len &&
	      memcmp(got.code, tu.code, tu.code_len) == 0, "code preserved");
	CHECK(got.reloc_count == 2 &&
	      got.relocs[1].guest_target == 0x402000, "relocs preserved");
	CHECK(got.exit_count == 2 && got.exits[0] == 0x401000, "exits preserved");
	CHECK(memcmp(got.guest_hash, tu.guest_hash, 32) == 0, "guest_hash preserved");

	oryx_tu_free(&got);
	free(b1); free(b2);
}

static void test_parse_rejects_bad(void)
{
	printf("test: parse rejects malformed input\n");
	struct oryx_tu tu; make_tu(&tu);
	uint8_t *b; size_t l;
	oryx_tu_serialize(&tu, &b, &l);

	struct oryx_tu got;
	CHECK(oryx_tu_parse(b, 10, &got) == ORYX_ERR_FORMAT, "reject truncated header");

	uint8_t save = b[0]; b[0] ^= 0xff;
	CHECK(oryx_tu_parse(b, l, &got) == ORYX_ERR_FORMAT, "reject bad magic");
	b[0] = save;

	CHECK(oryx_tu_parse(b, l - 1, &got) == ORYX_ERR_FORMAT, "reject wrong total length");
	free(b);
}

static void test_cache_roundtrip(void)
{
	printf("test: cache put/get TU by logical key\n");
	rm_rf(TESTDIR);
	struct oryx_cache c;
	CHECK(oryx_cache_open(&c, TESTDIR) == ORYX_OK, "cache open");

	struct oryx_tu tu; make_tu(&tu);
	char addr[65], key[65];
	oryx_tu_logical_key(&tu, key);
	CHECK(oryx_cache_put_tu(&c, &tu, addr) == ORYX_OK, "put_tu");

	char resolved[65];
	CHECK(oryx_cache_resolve(&c, key, resolved) == ORYX_OK, "resolve key");
	CHECK(strcmp(resolved, addr) == 0, "resolve matches put address");

	struct oryx_tu got;
	CHECK(oryx_cache_get_tu(&c, key, &got) == ORYX_OK, "get_tu");
	CHECK(got.profile_id == 7 && got.code_len == tu.code_len, "get_tu content correct");
	oryx_tu_free(&got);

	CHECK(oryx_cache_verify(&c, addr) == ORYX_OK, "verify intact blob");
}

static void test_integrity_fail_closed(void)
{
	printf("test: tampered blob is rejected (fail-closed)\n");
	rm_rf(TESTDIR);
	struct oryx_cache c;
	oryx_cache_open(&c, TESTDIR);

	struct oryx_tu tu; make_tu(&tu);
	char addr[65], key[65];
	oryx_tu_logical_key(&tu, key);
	oryx_cache_put_tu(&c, &tu, addr);

	/* Corrupt one byte of the stored blob directly on disk. */
	char blob_path[700];
	snprintf(blob_path, sizeof(blob_path), "%s/blobs/%s", TESTDIR, addr);
	int fd = open(blob_path, O_RDWR);
	CHECK(fd >= 0, "open blob for tamper");
	uint8_t byte;
	if (pread(fd, &byte, 1, 84) == 1) {   /* flip first code byte */
		byte ^= 0xff;
		if (pwrite(fd, &byte, 1, 84) != 1) printf("  (pwrite failed)\n");
	}
	close(fd);

	uint8_t *buf; size_t len;
	CHECK(oryx_cache_get_blob(&c, addr, &buf, &len) == ORYX_ERR_INTEGRITY,
	      "get_blob rejects tampered bytes");
	CHECK(oryx_cache_verify(&c, addr) == ORYX_ERR_INTEGRITY,
	      "verify rejects tampered blob");

	struct oryx_tu got;
	CHECK(oryx_cache_get_tu(&c, key, &got) == ORYX_ERR_INTEGRITY,
	      "get_tu refuses to parse tampered code");
}

static void test_missing(void)
{
	printf("test: missing key / blob -> NOTFOUND\n");
	rm_rf(TESTDIR);
	struct oryx_cache c;
	oryx_cache_open(&c, TESTDIR);

	char addr[65];
	CHECK(oryx_cache_resolve(&c,
		"0000000000000000000000000000000000000000000000000000000000000000",
		addr) == ORYX_ERR_NOTFOUND, "resolve missing key");

	uint8_t *buf; size_t len;
	CHECK(oryx_cache_get_blob(&c,
		"1111111111111111111111111111111111111111111111111111111111111111",
		&buf, &len) == ORYX_ERR_NOTFOUND, "get missing blob");
}

int main(void)
{
	test_sha256_vector();
	test_roundtrip_and_determinism();
	test_parse_rejects_bad();
	test_cache_roundtrip();
	test_integrity_fail_closed();
	test_missing();
	rm_rf(TESTDIR);

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
