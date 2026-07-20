// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryx-perf.c — no-root on-device performance probe for x86-64-on-ARM gaming.
 *
 * Samples, while a game runs (in GameNative / Winlator / any app), the things
 * that actually gate frame rate — per-core CPU utilisation + frequency, GPU
 * clock/busy, SoC temperatures, memory — and, on exit, prints a BOTTLENECK
 * VERDICT (CPU-emulation-bound vs GPU/driver-bound vs thermal-throttled). The
 * point is to replace "it feels slow" with evidence, so effort targets the real
 * limiter. Pair it with DXVK_HUD (fps,frametimes,gpuload) on-screen — see README.
 *
 * No root: reads only world-readable /proc and /sys. Whatever a path won't give
 * (some Adreno GPU counters are SELinux-locked without root) is reported as
 * "n/a" and the verdict degrades gracefully, leaning on the CPU-vs-FPS signal.
 *
 * Build:  make          (native, e.g. in Termux on the phone)
 * Run:    ./oryx-perf --secs 60 --hz 4 --csv run.csv [--fps 45]
 *         (start it, then play a demanding scene for the duration)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

#define MAXCPU 32

/* ---- small file readers (return -1 / NULL when unreadable) --------------- */
static long read_ll(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	long long v = -1;
	if (fscanf(f, "%lld", &v) != 1) v = -1;
	fclose(f);
	return (long)v;
}

/* First readable path from a NULL-terminated candidate list (records which). */
static const char *first_readable(const char *const *cands, long *out)
{
	for (int i = 0; cands[i]; i++) {
		long v = read_ll(cands[i]);
		if (v >= 0) { *out = v; return cands[i]; }
	}
	*out = -1;
	return NULL;
}

/* ---- /proc/stat per-cpu busy/idle jiffies -------------------------------- */
struct cpustat { unsigned long long busy, idle; };
static int read_cpustat(struct cpustat *st, int max)
{
	FILE *f = fopen("/proc/stat", "r");
	if (!f) return 0;
	char line[512];
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		int c;
		unsigned long long u, ni, s, id, io, irq, sirq, steal = 0;
		/* "cpuN user nice system idle iowait irq softirq steal ..." */
		if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
			   &c, &u, &ni, &s, &id, &io, &irq, &sirq, &steal) >= 8 &&
		    c >= 0 && c < max) {
			st[c].busy = u + ni + s + irq + sirq + steal;
			st[c].idle = id + io;
			if (c + 1 > n) n = c + 1;
		}
	}
	fclose(f);
	return n;
}

/* ---- GPU (Adreno/kgsl) — best-effort, may be root-locked ----------------- */
static const char *GPU_CLK[] = {
	"/sys/class/kgsl/kgsl-3d0/gpuclk",
	"/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
	NULL };
static const char *GPU_BUSY[] = {          /* an instantaneous 0..100 load */
	"/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
	"/sys/class/kgsl/kgsl-3d0/devfreq/gpu_load",
	NULL };

/* ---- thermals: hottest zone in milli-C ----------------------------------- */
static long hottest_temp_mC(void)
{
	long hot = -1;
	for (int z = 0; z < 40; z++) {
		char p[80];
		snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", z);
		long t = read_ll(p);
		if (t > hot) hot = t;
	}
	return hot;                                /* -1 if none readable */
}

static long mem_avail_kb(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) return -1;
	char k[64]; long v = -1; char u[16];
	while (fscanf(f, "%63s %ld %15s", k, &v, u) == 3)
		if (!strcmp(k, "MemAvailable:")) { fclose(f); return v; }
	fclose(f);
	return -1;
}

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
	long secs = 0;          /* 0 = until Ctrl-C */
	int  hz = 2;
	const char *csv_path = NULL;
	double user_fps = -1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--secs") && i+1 < argc) secs = atol(argv[++i]);
		else if (!strcmp(argv[i], "--hz") && i+1 < argc) hz = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--csv") && i+1 < argc) csv_path = argv[++i];
		else if (!strcmp(argv[i], "--fps") && i+1 < argc) user_fps = atof(argv[++i]);
		else { fprintf(stderr, "usage: %s [--secs N] [--hz H] [--csv F] [--fps observed]\n", argv[0]); return 2; }
	}
	if (hz < 1) hz = 1;

	signal(SIGINT, on_sigint);
	FILE *csv = csv_path ? fopen(csv_path, "w") : NULL;

	/* Probe which GPU counters are readable, once. */
	long gclk, gbusy;
	const char *gclk_p  = first_readable(GPU_CLK, &gclk);
	const char *gbusy_p = first_readable(GPU_BUSY, &gbusy);

	struct cpustat prev[MAXCPU] = {0}, cur[MAXCPU];
	int ncpu = read_cpustat(prev, MAXCPU);
	if (ncpu <= 0) { fprintf(stderr, "cannot read /proc/stat\n"); return 1; }

	printf("oryx-perf: %d CPUs; GPU clock %s; GPU busy %s; sampling at %d Hz%s\n",
	       ncpu, gclk_p ? "yes" : "n/a(no-root?)", gbusy_p ? "yes" : "n/a(no-root?)", hz,
	       secs ? "" : " (Ctrl-C to stop)");
	if (csv) {
		fprintf(csv, "t_ms,busiest_util,avg_util");
		for (int c = 0; c < ncpu; c++) fprintf(csv, ",cpu%d_util,cpu%d_khz", c, c);
		fprintf(csv, ",gpu_khz,gpu_busy,temp_mC,mem_avail_kb\n");
	}

	/* accumulators for the verdict */
	double sum_busiest = 0, sum_gpu = 0; int nsamp = 0, ngpu = 0;
	double peak_busiest = 0;
	long temp_max = -1;
	long first_clk_sum = -1, last_clk_sum = 0;  /* CPU freq trend (throttle) */
	int throttle_samples = 0;

	long start = now_ms();
	long interval_ms = 1000 / hz;

	while (!g_stop) {
		struct timespec req = { interval_ms/1000, (interval_ms%1000)*1000000L };
		nanosleep(&req, NULL);

		int n = read_cpustat(cur, MAXCPU);
		if (n < ncpu) ncpu = n;
		double busiest = 0, avgsum = 0;
		int used = 0;
		double per_util[MAXCPU]; long per_khz[MAXCPU];
		long clk_sum = 0;
		for (int c = 0; c < ncpu; c++) {
			unsigned long long db = cur[c].busy - prev[c].busy;
			unsigned long long di = cur[c].idle - prev[c].idle;
			unsigned long long tot = db + di;
			double util = tot ? (100.0 * (double)db / (double)tot) : 0.0;
			per_util[c] = util;
			if (util > busiest) busiest = util;
			avgsum += util; used++;
			char p[96];
			snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
			per_khz[c] = read_ll(p);
			if (per_khz[c] > 0) clk_sum += per_khz[c];
			prev[c] = cur[c];
		}
		double avg = used ? avgsum/used : 0;
		long gpu_k = gclk_p  ? read_ll(gclk_p)  : -1;
		long gpu_b = gbusy_p ? read_ll(gbusy_p) : -1;
		long temp  = hottest_temp_mC();
		long mem   = mem_avail_kb();

		sum_busiest += busiest; if (busiest > peak_busiest) peak_busiest = busiest;
		if (gpu_b >= 0) { sum_gpu += gpu_b; ngpu++; }
		if (temp > temp_max) temp_max = temp;
		if (first_clk_sum < 0 && clk_sum > 0) first_clk_sum = clk_sum;
		if (clk_sum > 0) last_clk_sum = clk_sum;
		if (first_clk_sum > 0 && clk_sum > 0 && clk_sum < first_clk_sum * 85 / 100) throttle_samples++;
		nsamp++;

		long t = now_ms() - start;
		char gbuf[32], tbuf[32];
		if (gpu_b >= 0) snprintf(gbuf, sizeof(gbuf), "%ld%%", gpu_b); else snprintf(gbuf, sizeof(gbuf), "n/a");
		if (temp  >= 0) snprintf(tbuf, sizeof(tbuf), "%.1fC", temp/1000.0); else snprintf(tbuf, sizeof(tbuf), "n/a");
		printf("\r[%4lds] busiest-core %5.1f%%  avg %5.1f%%  gpu %s  temp %s      ",
		       t/1000, busiest, avg, gbuf, tbuf);
		fflush(stdout);

		if (csv) {
			fprintf(csv, "%ld,%.1f,%.1f", t, busiest, avg);
			for (int c = 0; c < ncpu; c++) fprintf(csv, ",%.1f,%ld", per_util[c], per_khz[c]);
			fprintf(csv, ",%ld,%ld,%ld,%ld\n", gpu_k, gpu_b, temp, mem);
		}
		if (secs && t >= secs*1000) break;
	}
	if (csv) fclose(csv);
	printf("\n");

	if (nsamp == 0) { fprintf(stderr, "no samples collected\n"); return 1; }

	/* ---- verdict --------------------------------------------------------- */
	double mean_busiest = sum_busiest / nsamp;
	double mean_gpu = ngpu ? sum_gpu / ngpu : -1;
	int throttled = (first_clk_sum > 0 && last_clk_sum > 0 &&
			 (throttle_samples * 100 / nsamp) >= 20);

	printf("================= oryx-perf verdict =================\n");
	printf(" samples=%d  busiest-core: mean %.1f%% / peak %.1f%%\n", nsamp, mean_busiest, peak_busiest);
	if (mean_gpu >= 0) printf(" GPU busy: mean %.1f%%\n", mean_gpu);
	else               printf(" GPU busy: n/a (counter not readable without root)\n");
	if (temp_max >= 0) printf(" hottest zone: %.1f C%s\n", temp_max/1000.0, throttled ? "  (CPU freq dropped >=15%% -> THROTTLING)" : "");
	if (user_fps >= 0) printf(" observed FPS (you): %.1f\n", user_fps);
	printf("-----------------------------------------------------\n");

	const char *v, *why;
	if (mean_gpu >= 85.0) {
		v = "GPU / DRIVER BOUND";
		why = "GPU sat near 100%. CPU-emulation work (Box64/FEX/Oryx) will NOT raise FPS.\n"
		      "  Levers: GPU driver (the 8-Elite's Adreno needs Winlator's Vortek driver; Turnip\n"
		      "  doesn't support it yet), DXVK/Zink settings, in-game resolution/quality.";
	} else if (peak_busiest >= 90.0 && (mean_gpu < 0 || mean_gpu < 75.0)) {
		v = "CPU / EMULATION BOUND";
		why = "A core pins ~100% while the GPU isn't saturated -> the x86->ARM dynarec (or a\n"
		      "  single-threaded game loop) is the limiter. Levers: Box64/FEX tuning\n"
		      "  (BOX64_DYNAREC_*), a better/preloaded code cache, core affinity to a prime core.";
	} else if (throttled) {
		v = "THERMAL THROTTLED";
		why = "CPU frequency fell substantially as temps rose -> sustained perf is heat-limited,\n"
		      "  not compute-limited. Levers: cooling, power/thermal profile, lower settings.";
	} else if (user_fps >= 0 && user_fps < 50 && peak_busiest < 85 && mean_gpu < 0) {
		v = "LIKELY GPU BOUND (inferred)";
		why = "Low FPS but no CPU core is maxed and GPU load is unreadable -> the GPU/driver is\n"
		      "  the probable limiter. Confirm with DXVK_HUD gpuload (see README).";
	} else {
		v = "MIXED / INCONCLUSIVE";
		why = "No single resource is clearly pinned. Re-run on a heavier scene, add --fps from\n"
		      "  DXVK_HUD, and enable DXVK_HUD=gpuload to read GPU load directly.";
	}
	printf(" VERDICT: %s\n   %s\n", v, why);
	printf("=====================================================\n");
	return 0;
}
