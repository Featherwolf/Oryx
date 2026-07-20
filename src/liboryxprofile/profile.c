// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * profile.c — reference implementation of Oryx Part C auto-tuning profiles.
 */
#define _GNU_SOURCE
#include "profile.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers ------------------------------------------------------- */
static char *trim(char *s)
{
	while (*s && isspace((unsigned char)*s)) s++;
	if (!*s) return s;
	char *e = s + strlen(s) - 1;
	while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
	return s;
}

static void set_str(char *dst, const char *src)
{
	snprintf(dst, ORYX_STR, "%s", src);
}

void oryx_profile_init(struct oryx_profile *p)
{
	memset(p, 0, sizeof(*p));
	set_str(p->fsr_mode, "off");
	p->crash_rate = 0.0;
}

/* ---- parse --------------------------------------------------------------- */
int oryx_profile_parse(const char *text, size_t len, struct oryx_profile *out)
{
	if (!text || !out)
		return ORYX_P_INVAL;
	oryx_profile_init(out);

	char *copy = malloc(len + 1);
	if (!copy)
		return ORYX_P_ERR;
	memcpy(copy, text, len);
	copy[len] = '\0';

	char *save = NULL;
	for (char *line = strtok_r(copy, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *t = trim(line);
		if (!*t || *t == '#')
			continue;
		char *eq = strchr(t, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *k = trim(t);
		char *v = trim(eq + 1);

		if      (!strcmp(k, "game_build_hash")) set_str(out->game_build_hash, v);
		else if (!strcmp(k, "device_class"))    set_str(out->device_class, v);
		else if (!strcmp(k, "requires_hw_tso")) out->requires_hw_tso = atoi(v);
		else if (!strcmp(k, "backend"))
			out->backend = !strcmp(v, "fex") ? ORYX_BACKEND_FEX : ORYX_BACKEND_BOX64;
		else if (!strcmp(k, "memmodel"))
			out->memmodel = !strcmp(v, "hw_tso") ? ORYX_MM_HW_TSO : ORYX_MM_SW_FENCES;
		else if (!strcmp(k, "turnip_build"))    set_str(out->turnip_build, v);
		else if (!strcmp(k, "dxvk_opts"))       set_str(out->dxvk_opts, v);
		else if (!strcmp(k, "vkd3d_opts"))      set_str(out->vkd3d_opts, v);
		else if (!strcmp(k, "resolution"))      set_str(out->resolution, v);
		else if (!strcmp(k, "fsr_mode"))        set_str(out->fsr_mode, v);
		else if (!strcmp(k, "device_spoof"))    set_str(out->device_spoof, v);
		else if (!strcmp(k, "median_fps"))      out->median_fps = atof(v);
		else if (!strcmp(k, "p1_low_fps"))      out->p1_low_fps = atof(v);
		else if (!strcmp(k, "crash_rate"))      out->crash_rate = atof(v);
		else if (!strcmp(k, "sustained_fps"))   out->sustained_fps = atof(v);
		else if (!strcmp(k, "sample_count"))    out->sample_count = (uint32_t)strtoul(v, NULL, 10);
		/* unknown keys ignored for forward-compat */
	}
	free(copy);

	if (!out->game_build_hash[0] || !out->device_class[0])
		return ORYX_P_INVAL;
	return ORYX_P_OK;
}

/* ---- serialize ----------------------------------------------------------- */
int oryx_profile_serialize(const struct oryx_profile *p, char *buf, size_t bufsz)
{
	int n = snprintf(buf, bufsz,
		"game_build_hash = %s\n"
		"device_class = %s\n"
		"requires_hw_tso = %d\n"
		"backend = %s\n"
		"memmodel = %s\n"
		"turnip_build = %s\n"
		"dxvk_opts = %s\n"
		"vkd3d_opts = %s\n"
		"resolution = %s\n"
		"fsr_mode = %s\n"
		"device_spoof = %s\n"
		"median_fps = %.2f\n"
		"p1_low_fps = %.2f\n"
		"crash_rate = %.4f\n"
		"sustained_fps = %.2f\n"
		"sample_count = %u\n",
		p->game_build_hash, p->device_class, p->requires_hw_tso,
		p->backend == ORYX_BACKEND_FEX ? "fex" : "box64",
		p->memmodel == ORYX_MM_HW_TSO ? "hw_tso" : "sw_fences",
		p->turnip_build, p->dxvk_opts, p->vkd3d_opts, p->resolution,
		p->fsr_mode, p->device_spoof,
		p->median_fps, p->p1_low_fps, p->crash_rate, p->sustained_fps,
		p->sample_count);
	return (n < 0 || (size_t)n >= bufsz) ? ORYX_P_ERR : ORYX_P_OK;
}

/* ---- ranking ------------------------------------------------------------- */
double oryx_profile_score(const struct oryx_profile *p)
{
	const double prior_n = 10.0;      /* pseudo-count shrinking undersampled profiles */
	const double prior_crash = 0.10;  /* assume 10% crash until proven otherwise */

	double n = (double)p->sample_count;
	double conf = n / (n + prior_n);                       /* 0..1 */
	double eff_crash = (p->crash_rate * n + prior_crash * prior_n) / (n + prior_n);

	/* Smoothness (1%-low) matters most, then median, then thermal sustain. */
	double base = 0.5 * p->p1_low_fps
		    + 0.3 * p->median_fps
		    + 0.2 * p->sustained_fps;

	/* Crashing scales the whole thing down; undersampling never zeroes it out. */
	return base * (1.0 - eff_crash) * (0.5 + 0.5 * conf);
}

int oryx_profile_compatible(const struct oryx_profile *p, const struct oryx_caps *caps)
{
	int needs_tso = p->requires_hw_tso || (p->memmodel == ORYX_MM_HW_TSO);

	if (strcmp(p->device_class, caps->device_class) != 0)
		return 0;
	if (needs_tso && !caps->hw_tso)
		return 0;
	return 1;
}

/* ---- profile set --------------------------------------------------------- */
void oryx_profile_set_init(struct oryx_profile_set *s)
{
	s->items = NULL; s->count = 0; s->cap = 0;
}

int oryx_profile_set_add(struct oryx_profile_set *s, const struct oryx_profile *p)
{
	if (s->count == s->cap) {
		size_t nc = s->cap ? s->cap * 2 : 8;
		struct oryx_profile *ni = realloc(s->items, nc * sizeof(*ni));
		if (!ni)
			return ORYX_P_ERR;
		s->items = ni; s->cap = nc;
	}
	s->items[s->count++] = *p;
	return ORYX_P_OK;
}

void oryx_profile_set_free(struct oryx_profile_set *s)
{
	free(s->items);
	s->items = NULL; s->count = s->cap = 0;
}

static int ends_with(const char *s, const char *suf)
{
	size_t ls = strlen(s), lf = strlen(suf);
	return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

int oryx_profile_set_load_dir(struct oryx_profile_set *s, const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return ORYX_P_ERR;

	struct dirent *de;
	int loaded = 0;
	while ((de = readdir(d)) != NULL) {
		if (!ends_with(de->d_name, ".profile"))
			continue;
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);

		FILE *f = fopen(path, "rb");
		if (!f)
			continue;
		char text[4096];
		size_t n = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
		text[n] = '\0';

		struct oryx_profile p;
		if (oryx_profile_parse(text, n, &p) == ORYX_P_OK &&
		    oryx_profile_set_add(s, &p) == ORYX_P_OK)
			loaded++;
	}
	closedir(d);
	return loaded;
}

int oryx_profile_resolve(const struct oryx_profile_set *s,
			 const char *game_build_hash,
			 const struct oryx_caps *caps,
			 struct oryx_profile *out)
{
	const struct oryx_profile *best = NULL;
	double best_score = -1.0;

	for (size_t i = 0; i < s->count; i++) {
		const struct oryx_profile *p = &s->items[i];
		if (strcmp(p->game_build_hash, game_build_hash) != 0)
			continue;
		if (!oryx_profile_compatible(p, caps))
			continue;
		double sc = oryx_profile_score(p);
		if (sc > best_score ||
		    (sc == best_score && best && p->sample_count > best->sample_count)) {
			best = p; best_score = sc;
		}
	}
	if (!best)
		return ORYX_P_NOTFOUND;
	*out = *best;
	return ORYX_P_OK;
}

/* ---- apply --------------------------------------------------------------- */
int oryx_profile_apply_env(const struct oryx_profile *p, char *buf, size_t bufsz)
{
	size_t off = 0;
	#define EMIT(...) do { \
		int _n = snprintf(buf + off, bufsz - off, __VA_ARGS__); \
		if (_n < 0 || (size_t)_n >= bufsz - off) return ORYX_P_ERR; \
		off += (size_t)_n; \
	} while (0)

	EMIT("export ORYX_BACKEND=%s\n", p->backend == ORYX_BACKEND_FEX ? "fex" : "box64");

	if (p->memmodel == ORYX_MM_HW_TSO) {
		/* Hardware ordering: the emulator calls liboryxmm; no software knobs. */
		EMIT("export ORYX_MM_TSO=1\n");
		if (p->backend == ORYX_BACKEND_FEX)
			EMIT("export FEX_TSOENABLED=0\n");   /* safe now: hardware guarantees it */
	} else {
		/* Software ordering. */
		EMIT("export ORYX_MM_TSO=0\n");
		if (p->backend == ORYX_BACKEND_BOX64)
			EMIT("export BOX64_DYNAREC_STRONGMEM=1\n");
		else
			EMIT("export FEX_TSOENABLED=1\n");
	}

	if (p->turnip_build[0]) EMIT("export ORYX_TURNIP_BUILD=%s\n", p->turnip_build);
	if (p->dxvk_opts[0])    EMIT("export DXVK_CONFIG=\"%s\"\n", p->dxvk_opts);
	if (p->vkd3d_opts[0])   EMIT("export VKD3D_CONFIG=\"%s\"\n", p->vkd3d_opts);
	if (p->resolution[0])   EMIT("export ORYX_RES=%s\n", p->resolution);
	if (p->fsr_mode[0])     EMIT("export ORYX_FSR=%s\n", p->fsr_mode);
	if (p->device_spoof[0]) EMIT("export ORYX_GPU_SPOOF=\"%s\"\n", p->device_spoof);

	#undef EMIT
	return ORYX_P_OK;
}
