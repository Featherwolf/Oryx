/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * profile.h — Project Oryx Part C: auto-tuning profiles.
 *
 * A profile is the winning configuration for one game build on one device class,
 * crowd-sourced and ranked by real telemetry. The client resolves the best
 * profile compatible with the device's capabilities and applies it into a
 * GameNative-style per-game config, so users get community-best settings with
 * zero manual tuning.
 */
#ifndef ORYX_PROFILE_H
#define ORYX_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ORYX_P_OK = 0, ORYX_P_ERR = -1, ORYX_P_NOTFOUND = -2, ORYX_P_INVAL = -3 };

enum oryx_backend { ORYX_BACKEND_BOX64 = 0, ORYX_BACKEND_FEX = 1 };
enum oryx_memmodel { ORYX_MM_SW_FENCES = 0, ORYX_MM_HW_TSO = 1 };

#define ORYX_STR 96

struct oryx_profile {
	char game_build_hash[ORYX_STR];
	char device_class[ORYX_STR];     /* e.g. "SM8850" */

	int  requires_hw_tso;            /* 1 => only valid where hardware TSO exists */
	enum oryx_backend  backend;
	enum oryx_memmodel memmodel;

	char turnip_build[ORYX_STR];     /* driver build id (Part D) */
	char dxvk_opts[ORYX_STR];
	char vkd3d_opts[ORYX_STR];
	char resolution[ORYX_STR];       /* "1280x720" */
	char fsr_mode[ORYX_STR];         /* "off" | "quality" | "balanced" | "performance" */
	char device_spoof[ORYX_STR];     /* optional GPU spoof string, "" if none */

	/* Telemetry (aggregate, opt-in). */
	double median_fps;
	double p1_low_fps;               /* 1%-low: smoothness */
	double crash_rate;               /* 0..1 sessions ending in a crash */
	double sustained_fps;            /* fps after thermal throttling */
	uint32_t sample_count;           /* sessions behind these numbers */
};

/* Device capability the client resolves against. */
struct oryx_caps {
	char device_class[ORYX_STR];
	int  hw_tso;                     /* 1 if the Oryx driver offers hardware TSO */
};

/* ---- parse / serialize (simple deterministic "key = value" text) --------- */
void oryx_profile_init(struct oryx_profile *p);
int  oryx_profile_parse(const char *text, size_t len, struct oryx_profile *out);
int  oryx_profile_serialize(const struct oryx_profile *p, char *buf, size_t bufsz);

/* ---- ranking ------------------------------------------------------------- */
/*
 * Composite score. Prioritizes NOT crashing, then smoothness (1%-low), then
 * median, then sustained/thermal fps. Low sample counts are shrunk toward a
 * neutral prior so a single lucky session can't top a well-sampled profile.
 */
double oryx_profile_score(const struct oryx_profile *p);

/* True if a profile may run on a device with the given capabilities. */
int oryx_profile_compatible(const struct oryx_profile *p, const struct oryx_caps *caps);

/* ---- resolution (pick the best compatible profile for a game) ------------ */
struct oryx_profile_set {
	struct oryx_profile *items;
	size_t count;
	size_t cap;
};

void oryx_profile_set_init(struct oryx_profile_set *s);
int  oryx_profile_set_add(struct oryx_profile_set *s, const struct oryx_profile *p);
void oryx_profile_set_free(struct oryx_profile_set *s);

/* Load every *.profile file in a directory into the set. Returns count or <0. */
int oryx_profile_set_load_dir(struct oryx_profile_set *s, const char *dir);

/* Best-scoring compatible profile for the given game build + caps. */
int oryx_profile_resolve(const struct oryx_profile_set *s,
			 const char *game_build_hash,
			 const struct oryx_caps *caps,
			 struct oryx_profile *out);

/* ---- apply --------------------------------------------------------------- */
/*
 * Render the profile into a GameNative-style environment/config as newline-
 * separated `export KEY=VALUE` lines. When hardware TSO is chosen, emits
 * ORYX_MM_TSO=1 (the emulator then calls liboryxmm) and omits STRONGMEM;
 * otherwise sets the software-ordering knobs.
 */
int oryx_profile_apply_env(const struct oryx_profile *p, char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif

#endif /* ORYX_PROFILE_H */
