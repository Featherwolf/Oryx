// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryxcache_cli.c — command-line front-end to liboryxcache. Useful for shader
 * blobs and for inspecting/verifying a cache. Translation units are produced by
 * the emulator, not here.
 *
 *   oryxcache key    <string>                  print SHA-256 of a string (make a logical key)
 *   oryxcache put    <dir> <logical_key> <file> store file as a blob under key; print address
 *   oryxcache resolve<dir> <logical_key>        print the content address for a key
 *   oryxcache get    <dir> <address> <outfile>  verify + write a blob to outfile
 *   oryxcache verify <dir> <address>            check a blob's integrity
 */
#define _GNU_SOURCE
#include "oryxcache.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int slurp(const char *path, uint8_t **out, size_t *len)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	struct stat st;
	if (fstat(fd, &st) != 0) { close(fd); return -1; }
	uint8_t *b = malloc(st.st_size ? st.st_size : 1);
	if (!b) { close(fd); return -1; }
	size_t got = 0;
	while (got < (size_t)st.st_size) {
		ssize_t n = read(fd, b + got, st.st_size - got);
		if (n <= 0) break;
		got += n;
	}
	close(fd);
	*out = b; *len = got;
	return 0;
}

static const char *errstr(int rc)
{
	switch (rc) {
	case ORYX_OK: return "ok";
	case ORYX_ERR_IO: return "io error";
	case ORYX_ERR_FORMAT: return "bad format";
	case ORYX_ERR_INTEGRITY: return "INTEGRITY FAILURE";
	case ORYX_ERR_NOTFOUND: return "not found";
	case ORYX_ERR_INVAL: return "invalid argument";
	case ORYX_ERR_NOMEM: return "out of memory";
	default: return "unknown";
	}
}

static int usage(void)
{
	fprintf(stderr,
		"usage:\n"
		"  oryxcache key     <string>\n"
		"  oryxcache put     <dir> <logical_key> <file>\n"
		"  oryxcache resolve <dir> <logical_key>\n"
		"  oryxcache get     <dir> <address> <outfile>\n"
		"  oryxcache verify  <dir> <address>\n");
	return 2;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return usage();

	if (!strcmp(argv[1], "key") && argc == 3) {
		uint8_t d[32]; char hex[65];
		sha256(argv[2], strlen(argv[2]), d);
		sha256_hex(d, hex);
		printf("%s\n", hex);
		return 0;
	}

	if (!strcmp(argv[1], "put") && argc == 5) {
		struct oryx_cache c;
		if (oryx_cache_open(&c, argv[2]) != ORYX_OK) { fprintf(stderr, "open failed\n"); return 1; }
		uint8_t *data; size_t len;
		if (slurp(argv[4], &data, &len) != 0) { fprintf(stderr, "read %s failed\n", argv[4]); return 1; }
		char addr[65];
		int rc = oryx_cache_put_blob(&c, data, len, argv[3], addr);
		free(data);
		if (rc != ORYX_OK) { fprintf(stderr, "put: %s\n", errstr(rc)); return 1; }
		printf("%s\n", addr);
		return 0;
	}

	if (!strcmp(argv[1], "resolve") && argc == 4) {
		struct oryx_cache c;
		if (oryx_cache_open(&c, argv[2]) != ORYX_OK) { fprintf(stderr, "open failed\n"); return 1; }
		char addr[65];
		int rc = oryx_cache_resolve(&c, argv[3], addr);
		if (rc != ORYX_OK) { fprintf(stderr, "resolve: %s\n", errstr(rc)); return 1; }
		printf("%s\n", addr);
		return 0;
	}

	if (!strcmp(argv[1], "get") && argc == 5) {
		struct oryx_cache c;
		if (oryx_cache_open(&c, argv[2]) != ORYX_OK) { fprintf(stderr, "open failed\n"); return 1; }
		uint8_t *buf; size_t len;
		int rc = oryx_cache_get_blob(&c, argv[3], &buf, &len);
		if (rc != ORYX_OK) { fprintf(stderr, "get: %s\n", errstr(rc)); return 1; }
		int fd = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) { fprintf(stderr, "open %s failed\n", argv[4]); free(buf); return 1; }
		if (write(fd, buf, len) != (ssize_t)len) { fprintf(stderr, "write failed\n"); }
		close(fd); free(buf);
		printf("wrote %zu bytes (integrity verified)\n", len);
		return 0;
	}

	if (!strcmp(argv[1], "verify") && argc == 4) {
		struct oryx_cache c;
		if (oryx_cache_open(&c, argv[2]) != ORYX_OK) { fprintf(stderr, "open failed\n"); return 1; }
		int rc = oryx_cache_verify(&c, argv[3]);
		printf("%s: %s\n", argv[3], errstr(rc));
		return rc == ORYX_OK ? 0 : 1;
	}

	return usage();
}
