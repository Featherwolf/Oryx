// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryxprofile_cli.c — resolve and apply Oryx tuning profiles.
 *
 *   oryxprofile resolve <dir> <game_build_hash> <device_class> <hw_tso 0|1>
 *       -> prints the winning profile, its score, and the env to apply
 *   oryxprofile apply   <file>     -> prints the env for a single profile
 *   oryxprofile score   <file>     -> prints a profile's score
 */
#define _GNU_SOURCE
#include "profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_profile(const char *path, struct oryx_profile *p)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	char text[4096];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = '\0';
	return oryx_profile_parse(text, n, p) == ORYX_P_OK ? 0 : -1;
}

static int usage(void)
{
	fprintf(stderr,
		"usage:\n"
		"  oryxprofile resolve <dir> <game_build_hash> <device_class> <hw_tso 0|1>\n"
		"  oryxprofile apply   <file>\n"
		"  oryxprofile score   <file>\n");
	return 2;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return usage();

	if (!strcmp(argv[1], "resolve") && argc == 6) {
		struct oryx_profile_set set;
		oryx_profile_set_init(&set);
		int n = oryx_profile_set_load_dir(&set, argv[2]);
		if (n < 0) { fprintf(stderr, "cannot read profile dir %s\n", argv[2]); return 1; }

		struct oryx_caps caps;
		snprintf(caps.device_class, sizeof(caps.device_class), "%s", argv[4]);
		caps.hw_tso = atoi(argv[5]);

		struct oryx_profile best;
		int rc = oryx_profile_resolve(&set, argv[3], &caps, &best);
		oryx_profile_set_free(&set);
		if (rc == ORYX_P_NOTFOUND) {
			fprintf(stderr, "no compatible profile for %s on %s (hw_tso=%d)\n",
				argv[3], argv[4], caps.hw_tso);
			return 1;
		}

		char ser[2048], env[2048];
		oryx_profile_serialize(&best, ser, sizeof(ser));
		oryx_profile_apply_env(&best, env, sizeof(env));
		printf("# selected profile (score=%.2f):\n%s\n# apply:\n%s",
		       oryx_profile_score(&best), ser, env);
		return 0;
	}

	if (!strcmp(argv[1], "apply") && argc == 3) {
		struct oryx_profile p;
		if (read_profile(argv[2], &p) != 0) { fprintf(stderr, "bad profile\n"); return 1; }
		char env[2048];
		oryx_profile_apply_env(&p, env, sizeof(env));
		fputs(env, stdout);
		return 0;
	}

	if (!strcmp(argv[1], "score") && argc == 3) {
		struct oryx_profile p;
		if (read_profile(argv[2], &p) != 0) { fprintf(stderr, "bad profile\n"); return 1; }
		printf("%.2f\n", oryx_profile_score(&p));
		return 0;
	}

	return usage();
}
