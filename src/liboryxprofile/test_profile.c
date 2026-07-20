// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * test_profile.c — unit tests for Oryx Part C profile engine.
 */
#define _GNU_SOURCE
#include "profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_pass, g_fail;
#define CHECK(cond, msg) do { \
	if (cond) g_pass++; \
	else { g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static const char *SW =
	"game_build_hash = cyberpunk2077_v210\n"
	"device_class = SM8850\n"
	"backend = box64\n"
	"memmodel = sw_fences\n"
	"turnip_build = mesa-26.1-a840\n"
	"resolution = 1280x720\n"
	"fsr_mode = quality\n"
	"median_fps = 48\n"
	"p1_low_fps = 31\n"
	"crash_rate = 0.05\n"
	"sustained_fps = 40\n"
	"sample_count = 400\n";

static const char *HWTSO =
	"game_build_hash = cyberpunk2077_v210\n"
	"device_class = SM8850\n"
	"requires_hw_tso = 1\n"
	"backend = box64\n"
	"memmodel = hw_tso\n"
	"turnip_build = mesa-26.1-a840\n"
	"resolution = 1280x720\n"
	"fsr_mode = quality\n"
	"median_fps = 60\n"
	"p1_low_fps = 52\n"
	"crash_rate = 0.01\n"
	"sustained_fps = 55\n"
	"sample_count = 250\n";

static void test_parse_serialize(void)
{
	printf("test: parse + serialize round-trip\n");
	struct oryx_profile p;
	CHECK(oryx_profile_parse(SW, strlen(SW), &p) == ORYX_P_OK, "parse SW");
	CHECK(strcmp(p.game_build_hash, "cyberpunk2077_v210") == 0, "game hash");
	CHECK(p.backend == ORYX_BACKEND_BOX64, "backend box64");
	CHECK(p.memmodel == ORYX_MM_SW_FENCES, "memmodel sw_fences");
	CHECK(p.p1_low_fps == 31.0, "p1_low parsed");
	CHECK(p.sample_count == 400, "sample_count parsed");

	char buf[2048];
	CHECK(oryx_profile_serialize(&p, buf, sizeof(buf)) == ORYX_P_OK, "serialize");
	struct oryx_profile p2;
	CHECK(oryx_profile_parse(buf, strlen(buf), &p2) == ORYX_P_OK, "re-parse");
	CHECK(p2.sample_count == p.sample_count &&
	      p2.median_fps == p.median_fps, "round-trip preserves values");
}

static void test_scoring(void)
{
	printf("test: scoring prioritizes stability + smoothness\n");
	struct oryx_profile sw, hw;
	oryx_profile_parse(SW, strlen(SW), &sw);
	oryx_profile_parse(HWTSO, strlen(HWTSO), &hw);
	CHECK(oryx_profile_score(&hw) > oryx_profile_score(&sw),
	      "hw_tso (higher fps, fewer crashes) outscores sw_fences");

	/* A crashy clone of hw should score well below it. */
	struct oryx_profile crashy = hw;
	crashy.crash_rate = 0.40;
	CHECK(oryx_profile_score(&crashy) < oryx_profile_score(&hw),
	      "high crash rate tanks the score");

	/* An undersampled clone scores below the well-sampled original. */
	struct oryx_profile fresh = hw;
	fresh.sample_count = 1;
	CHECK(oryx_profile_score(&fresh) < oryx_profile_score(&hw),
	      "undersampled profile ranks lower (confidence shrink)");
}

static void test_compat_and_resolve(void)
{
	printf("test: capability-aware resolution\n");
	struct oryx_profile sw, hw;
	oryx_profile_parse(SW, strlen(SW), &sw);
	oryx_profile_parse(HWTSO, strlen(HWTSO), &hw);

	struct oryx_profile_set set;
	oryx_profile_set_init(&set);
	oryx_profile_set_add(&set, &sw);
	oryx_profile_set_add(&set, &hw);

	struct oryx_caps caps_tso  = { .device_class = "SM8850", .hw_tso = 1 };
	struct oryx_caps caps_notso = { .device_class = "SM8850", .hw_tso = 0 };
	struct oryx_caps caps_other = { .device_class = "SM8650", .hw_tso = 1 };

	CHECK(oryx_profile_compatible(&hw, &caps_tso) == 1, "hw compatible with TSO device");
	CHECK(oryx_profile_compatible(&hw, &caps_notso) == 0, "hw incompatible without TSO");
	CHECK(oryx_profile_compatible(&sw, &caps_notso) == 1, "sw compatible without TSO");

	struct oryx_profile out;
	CHECK(oryx_profile_resolve(&set, "cyberpunk2077_v210", &caps_tso, &out) == ORYX_P_OK &&
	      out.memmodel == ORYX_MM_HW_TSO, "TSO device resolves to hw profile");
	CHECK(oryx_profile_resolve(&set, "cyberpunk2077_v210", &caps_notso, &out) == ORYX_P_OK &&
	      out.memmodel == ORYX_MM_SW_FENCES, "non-TSO device resolves to sw profile");
	CHECK(oryx_profile_resolve(&set, "cyberpunk2077_v210", &caps_other, &out) == ORYX_P_NOTFOUND,
	      "wrong device class -> not found");
	CHECK(oryx_profile_resolve(&set, "no_such_game", &caps_tso, &out) == ORYX_P_NOTFOUND,
	      "unknown game -> not found");

	oryx_profile_set_free(&set);
}

static void test_apply(void)
{
	printf("test: apply_env output\n");
	struct oryx_profile sw, hw;
	oryx_profile_parse(SW, strlen(SW), &sw);
	oryx_profile_parse(HWTSO, strlen(HWTSO), &hw);

	char env[2048];
	CHECK(oryx_profile_apply_env(&hw, env, sizeof(env)) == ORYX_P_OK, "apply hw");
	CHECK(strstr(env, "ORYX_MM_TSO=1") != NULL, "hw sets ORYX_MM_TSO=1");
	CHECK(strstr(env, "STRONGMEM") == NULL, "hw omits STRONGMEM");

	CHECK(oryx_profile_apply_env(&sw, env, sizeof(env)) == ORYX_P_OK, "apply sw");
	CHECK(strstr(env, "ORYX_MM_TSO=0") != NULL, "sw sets ORYX_MM_TSO=0");
	CHECK(strstr(env, "BOX64_DYNAREC_STRONGMEM=1") != NULL, "sw sets STRONGMEM for box64");
	CHECK(strstr(env, "ORYX_FSR=quality") != NULL, "fsr mode emitted");
}

static void test_load_dir(void)
{
	printf("test: load a directory of profiles\n");
	const char *dir = "./_testprofiles";
	char cmd[256]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
	if (system(cmd) != 0) {}

	char path[300];
	snprintf(path, sizeof(path), "%s/sw.profile", dir);
	FILE *f = fopen(path, "wb"); if (f){ fputs(SW, f); fclose(f);}
	snprintf(path, sizeof(path), "%s/hw.profile", dir);
	f = fopen(path, "wb"); if (f){ fputs(HWTSO, f); fclose(f);}

	struct oryx_profile_set set;
	oryx_profile_set_init(&set);
	int n = oryx_profile_set_load_dir(&set, dir);
	CHECK(n == 2, "loaded 2 profiles from dir");
	oryx_profile_set_free(&set);

	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	if (system(cmd) != 0) {}
}

int main(void)
{
	test_parse_serialize();
	test_scoring();
	test_compat_and_resolve();
	test_apply();
	test_load_dir();
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
