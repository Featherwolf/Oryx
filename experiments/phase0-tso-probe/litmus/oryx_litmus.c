/*
 * oryx_litmus.c — memory-model detector for Project Oryx, Phase 0.
 *
 * Decides whether the *effective* memory model on a given pair of cores is weakly
 * ordered (ARM default) or Total Store Order (x86 / Apple-TSO / a hypothetical
 * Oryon-TSO mode). Two tests, run as a matched pair, make the result rigorous:
 *
 *   MP  — Message Passing        distinguishes weak from TSO
 *   SB  — Store Buffer           positive control: proves the harness is sensitive
 *
 * ---- MP (Message Passing) --------------------------------------------------
 *      T0 (writer)          T1 (reader)
 *      x = 1                r1 = y
 *      y = 1                r2 = x
 *   Witness outcome:  r1 == 1 && r2 == 0   ("saw the flag but not the data")
 *      - ALLOWED under ARM weak ordering (store-store / load-load reordering)
 *      - FORBIDDEN under x86 TSO
 *   So on this pair:  MP witnesses > 0  => WEAK ;  MP witnesses == 0 => (maybe) TSO
 *
 * ---- SB (Store Buffer) -----------------------------------------------------
 *      T0                   T1
 *      x = 1                y = 1
 *      r0 = y               r1 = x
 *   Witness outcome:  r0 == 0 && r1 == 0   (each core reads the other's stale value)
 *      - ALLOWED under BOTH x86 TSO and ARM (store->load reordering / store buffer)
 *   So SB witnesses > 0 on ANY real out-of-order core. SB is the sensitivity control:
 *   if SB == 0 the harness simply isn't exposing reordering (timing too lockstep,
 *   affinity wrong, iters too low) and a "MP == 0" result is MEANINGLESS.
 *
 * ---- Decision rule (see run_phase0.sh) -------------------------------------
 *   SB(relaxed)  > 0   AND   MP(relaxed) > 0            -> WEAK model on this pair
 *   SB(relaxed)  > 0   AND   MP(relaxed) == 0           -> TSO in effect on this pair
 *   SB(relaxed) == 0                                    -> INCONCLUSIVE (insensitive)
 *   MP(fenced)  == 0   (always, sanity)                 -> harness discriminates
 *
 * The '--mode fenced' variant inserts a DMB ISH (real barrier) and must drive every
 * witness count to zero; it validates that the machinery can detect ordering at all.
 *
 * Build: see Makefile. Runs on aarch64 (real target) and on any host for self-test.
 */

#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <errno.h>
#include <sys/prctl.h>

/*
 * No-root hardware-TSO request path.
 *
 * If the running kernel implements a memory-model prctl (the ARM-Linux
 * "PR_SET_MEM_MODEL" interface, as on Asahi), an UNPRIVILEGED thread can ask the
 * kernel to switch it into hardware TSO — no root needed. These values mirror
 * that proposed interface; if a kernel ships different numbers, match them here.
 * On kernels without the feature the call simply returns -1/EINVAL and we fall
 * back to measuring the plain weak baseline.
 */
#ifndef PR_GET_MEM_MODEL
#define PR_GET_MEM_MODEL 0x6d4d444c
#define PR_SET_MEM_MODEL 0x6d4d444d
#endif
#ifndef PR_MEM_MODEL_DEFAULT
#define PR_MEM_MODEL_DEFAULT 0
#endif
#ifndef PR_MEM_MODEL_TSO
#define PR_MEM_MODEL_TSO 1
#endif

#define CACHELINE 128

/* Each shared variable sits alone on a cache line to widen the reordering window. */
typedef struct {
    _Alignas(CACHELINE) atomic_int x;
    _Alignas(CACHELINE) atomic_int y;
    _Alignas(CACHELINE) atomic_int t0_a;   /* T0's observed load (SB) */
    atomic_int t0_b;
    _Alignas(CACHELINE) atomic_int t1_a;   /* T1's observed load(s) (MP/SB/WRC r1) */
    atomic_int t1_b;
    _Alignas(CACHELINE) atomic_int t2_a;   /* T2's observed loads (WRC r2, r3) */
    atomic_int t2_b;
    _Alignas(CACHELINE) atomic_long gate_go;
    _Alignas(CACHELINE) atomic_long t0_done;
    _Alignas(CACHELINE) atomic_long t1_done;
    _Alignas(CACHELINE) atomic_long t2_done;
} shared_t;

/* MP/SB are 2-thread; WRC is 3-thread and probes MULTI-COPY ATOMICITY:
 *   T0: x=1        T1: r1=x; y=1        T2: r2=y; r3=x
 *   witness = r1==1 && r2==1 && r3==0  ("T1 saw x and published y; T2 saw y but
 *   not x" — a non-multi-copy-atomic / non-transitive observation).
 * FORBIDDEN under x86-TSO (MCA). On ARM it is forbidden ONLY if the reader loads
 * are ordered AND write-atomicity holds — so it is the decisive test for whether
 * LDAPR/STLR (rcpc) preserves MCA (the open question the 2-thread SB test cannot
 * answer). plain (no ordering) fires it on weak ARM = the sensitivity control. */
enum test_kind { TEST_MP, TEST_SB, TEST_WRC };

static shared_t *S;
static long g_iters  = 5000000L;   /* mobile-friendly default; raise for more sensitivity */
static int  g_fenced = 0;
static int  g_cpu_a  = 0;
static int  g_cpu_b  = 1;
static int  g_cpu_c  = 2;           /* third core, WRC only */
static int  g_json   = 0;
static int  g_request_tso = 0;     /* ask the kernel for hardware TSO (no root) */
static enum test_kind g_test = TEST_MP;

/* Per-thread: try to enter hardware TSO via the kernel. Returns 0 on success. */
static atomic_int g_tso_granted = 0;   /* count of threads the kernel put in TSO mode */
static int try_request_tso(void)
{
	int rc = prctl(PR_SET_MEM_MODEL, PR_MEM_MODEL_TSO, 0, 0, 0);
	if (rc == 0)
		atomic_fetch_add_explicit(&g_tso_granted, 1, memory_order_relaxed);
	return rc;
}

static void pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        fprintf(stderr, "warning: could not pin to CPU %d (continuing unpinned)\n", cpu);
}

static inline void barrier_full(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dmb ish" ::: "memory");   /* the real ARM barrier under test */
#else
    atomic_thread_fence(memory_order_seq_cst);       /* dev-host fallback so it still builds */
#endif
}

/*
 * ---- Mapping primitives (Layer A: validate the TSO->AArch64 *mapping*) -----
 *
 * The bare MP/SB tests above measure the hardware. Layer A instead lowers the
 * two shared accesses using the exact instruction sequences a translator would
 * emit for a SHARED x86 access, so the litmus outcome validates the *mapping*:
 *
 *   plain : STR             / LDR              weak — the untranslated baseline
 *   rcpc  : STLR            / LDAPR            EXACT x86-TSO (needs FEAT_LRCPC, 8.3)
 *   sc    : STLR            / LDAR             RCsc — STRONGER than TSO (over-strong)
 *   dmb   : DMB ISHST; STR  / LDR; DMB ISHLD   EXACT x86-TSO via fences (pre-8.3)
 *
 * The decisive, on-device signatures (see run_layerA.sh):
 *   - MP-forbidden (r1==1 && r2==0) must be ZERO under rcpc, sc AND dmb: all
 *     three restore the store->store / load->load ordering x86 requires.
 *   - SB-allowed (r0==0 && r1==0) must be NONZERO under rcpc and dmb (they keep
 *     the W->R store-buffer relaxation TSO permits) but ZERO under sc (which
 *     also forbids W->R). Observing SB under rcpc is the empirical proof that
 *     LDAPR/STLR is exact-TSO, not accidentally sequentially consistent.
 *
 * On a non-aarch64 host these fall back to C11 atomics of matching strength so
 * the tool builds and its control flow self-tests; the rcpc-vs-sc SB contrast
 * is an ARM-weak-ordering phenomenon and only appears on the real device.
 */
enum map_kind { MAP_PLAIN = 0, MAP_RCPC, MAP_SC, MAP_DMB };

#if defined(__aarch64__)
static inline void st_plain(atomic_int *p, int v)
{ __asm__ __volatile__("str %w0, [%1]" :: "r"(v), "r"(p) : "memory"); }
static inline int  ld_plain(atomic_int *p)
{ int v; __asm__ __volatile__("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
static inline void st_release(atomic_int *p, int v)      /* STLR (rcpc + sc stores) */
{ __asm__ __volatile__("stlr %w0, [%1]" :: "r"(v), "r"(p) : "memory"); }
static inline int  ld_acq_sc(atomic_int *p)              /* LDAR  (RCsc acquire)    */
{ int v; __asm__ __volatile__("ldar %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
static inline int  ld_acq_pc(atomic_int *p)              /* LDAPR (RCpc acquire)    */
{ int v; __asm__ __volatile__(".arch_extension rcpc\n\tldapr %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
static inline void dmb_ishst(void){ __asm__ __volatile__("dmb ishst" ::: "memory"); }
static inline void dmb_ishld(void){ __asm__ __volatile__("dmb ishld" ::: "memory"); }
#else
static inline void st_plain(atomic_int *p, int v){ atomic_store_explicit(p, v, memory_order_relaxed); }
static inline int  ld_plain(atomic_int *p){ return atomic_load_explicit(p, memory_order_relaxed); }
static inline void st_release(atomic_int *p, int v){ atomic_store_explicit(p, v, memory_order_release); }
static inline int  ld_acq_sc(atomic_int *p){ return atomic_load_explicit(p, memory_order_acquire); }
static inline int  ld_acq_pc(atomic_int *p){ return atomic_load_explicit(p, memory_order_acquire); }
static inline void dmb_ishst(void){ atomic_thread_fence(memory_order_release); }
static inline void dmb_ishld(void){ atomic_thread_fence(memory_order_acquire); }
#endif

static enum map_kind g_map = MAP_PLAIN;

/* Lower one SHARED store/load per the selected mapping. */
static inline void map_store(atomic_int *p, int v)
{
    switch (g_map) {
    case MAP_RCPC: case MAP_SC: st_release(p, v); break;   /* STLR */
    case MAP_DMB:  dmb_ishst(); st_plain(p, v);  break;    /* DMB ISHST; STR */
    default:       st_plain(p, v);               break;    /* STR */
    }
}
static inline int map_load(atomic_int *p)
{
    switch (g_map) {
    case MAP_RCPC: return ld_acq_pc(p);                       /* LDAPR */
    case MAP_SC:   return ld_acq_sc(p);                       /* LDAR  */
    case MAP_DMB:  { int v = ld_plain(p); dmb_ishld(); return v; }  /* LDR; DMB ISHLD */
    default:       return ld_plain(p);                        /* LDR */
    }
}
static const char *map_name(enum map_kind m)
{
    switch (m) {
    case MAP_RCPC: return "rcpc"; case MAP_SC: return "sc";
    case MAP_DMB:  return "dmb";  default:     return "plain";
    }
}

static inline void wait_gate(atomic_long *gate, long target)
{
    while (atomic_load_explicit(gate, memory_order_acquire) < target)
        ; /* spin */
}

/* Thread 0. MP: writer (x=1; y=1). SB: x=1; r0=y. */
static void *thread0(void *arg)
{
    (void)arg;
    pin_to_cpu(g_cpu_a);
    if (g_request_tso) try_request_tso();
    for (long i = 1; i <= g_iters; i++) {
        wait_gate(&S->gate_go, i);
        if (g_test == TEST_MP) {
            map_store(&S->x, 1);
            if (g_map == MAP_PLAIN && g_fenced) barrier_full();
            map_store(&S->y, 1);
        } else if (g_test == TEST_WRC) {   /* T0: x=1 */
            map_store(&S->x, 1);
        } else { /* SB */
            map_store(&S->x, 1);
            if (g_map == MAP_PLAIN && g_fenced) barrier_full();
            int r0 = map_load(&S->y);
            atomic_store_explicit(&S->t0_a, r0, memory_order_relaxed);
        }
        atomic_store_explicit(&S->t0_done, i, memory_order_release);
    }
    return NULL;
}

/* Thread 1. MP: reader (r1=y; r2=x). SB: y=1; r1=x. */
static void *thread1(void *arg)
{
    (void)arg;
    pin_to_cpu(g_cpu_b);
    if (g_request_tso) try_request_tso();
    for (long i = 1; i <= g_iters; i++) {
        wait_gate(&S->gate_go, i);
        if (g_test == TEST_MP) {
            int r1 = map_load(&S->y);
            if (g_map == MAP_PLAIN && g_fenced) barrier_full();
            int r2 = map_load(&S->x);
            atomic_store_explicit(&S->t1_a, r1, memory_order_relaxed);
            atomic_store_explicit(&S->t1_b, r2, memory_order_relaxed);
        } else if (g_test == TEST_WRC) {   /* T1: r1=x; y=1 (load ordered before store) */
            int r1 = map_load(&S->x);
            atomic_store_explicit(&S->t1_a, r1, memory_order_relaxed);
            map_store(&S->y, 1);
        } else { /* SB */
            map_store(&S->y, 1);
            if (g_map == MAP_PLAIN && g_fenced) barrier_full();
            int r1 = map_load(&S->x);
            atomic_store_explicit(&S->t1_a, r1, memory_order_relaxed);
        }
        atomic_store_explicit(&S->t1_done, i, memory_order_release);
    }
    return NULL;
}

/* Thread 2 (WRC reader only). r2=y; r3=x. Witness needs r2==1 && r3==0. */
static void *thread2(void *arg)
{
    (void)arg;
    pin_to_cpu(g_cpu_c);
    if (g_request_tso) try_request_tso();
    for (long i = 1; i <= g_iters; i++) {
        wait_gate(&S->gate_go, i);
        int r2 = map_load(&S->y);
        int r3 = map_load(&S->x);
        atomic_store_explicit(&S->t2_a, r2, memory_order_relaxed);
        atomic_store_explicit(&S->t2_b, r3, memory_order_relaxed);
        atomic_store_explicit(&S->t2_done, i, memory_order_release);
    }
    return NULL;
}

static void parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--test") && i + 1 < argc) {
            const char *t = argv[++i];
            if      (!strcmp(t, "mp"))  g_test = TEST_MP;
            else if (!strcmp(t, "sb"))  g_test = TEST_SB;
            else if (!strcmp(t, "wrc")) g_test = TEST_WRC;
            else { fprintf(stderr, "unknown --test %s (mp|sb|wrc)\n", t); exit(2); }
        } else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            g_fenced = !strcmp(argv[++i], "fenced");
        } else if (!strcmp(argv[i], "--map") && i + 1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "plain")) g_map = MAP_PLAIN;
            else if (!strcmp(m, "rcpc"))  g_map = MAP_RCPC;
            else if (!strcmp(m, "sc"))    g_map = MAP_SC;
            else if (!strcmp(m, "dmb"))   g_map = MAP_DMB;
            else { fprintf(stderr, "unknown --map %s (plain|rcpc|sc|dmb)\n", m); exit(2); }
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            g_iters = strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--cpu-a") && i + 1 < argc) {
            g_cpu_a = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--cpu-b") && i + 1 < argc) {
            g_cpu_b = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--cpu-c") && i + 1 < argc) {
            g_cpu_c = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--json")) {
            g_json = 1;
        } else if (!strcmp(argv[i], "--request-tso")) {
            g_request_tso = 1;
        } else {
            fprintf(stderr,
                "usage: %s [--test mp|sb|wrc] [--mode relaxed|fenced] "
                "[--map plain|rcpc|sc|dmb] [--iters N] "
                "[--cpu-a A] [--cpu-b B] [--cpu-c C] [--json] [--request-tso]\n", argv[0]);
            exit(2);
        }
    }
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);

    if (posix_memalign((void **)&S, CACHELINE, sizeof(shared_t)) != 0 || !S) {
        perror("posix_memalign"); return 1;
    }
    memset(S, 0, sizeof(*S));

    int is_wrc = (g_test == TEST_WRC);

    pthread_t th0, th1, th2;
    if (pthread_create(&th0, NULL, thread0, NULL) ||
        pthread_create(&th1, NULL, thread1, NULL) ||
        (is_wrc && pthread_create(&th2, NULL, thread2, NULL))) {
        perror("pthread_create"); return 1;
    }

    long witnesses = 0;   /* the reordering outcome of interest for the chosen test */
    long total     = 0;

    for (long i = 1; i <= g_iters; i++) {
        atomic_store_explicit(&S->x, 0, memory_order_relaxed);
        atomic_store_explicit(&S->y, 0, memory_order_relaxed);

        atomic_store_explicit(&S->gate_go, i, memory_order_release);

        wait_gate(&S->t0_done, i);
        wait_gate(&S->t1_done, i);
        if (is_wrc) wait_gate(&S->t2_done, i);

        if (g_test == TEST_MP) {
            int r1 = atomic_load_explicit(&S->t1_a, memory_order_relaxed);
            int r2 = atomic_load_explicit(&S->t1_b, memory_order_relaxed);
            if (r1 == 1 && r2 == 0) witnesses++;   /* forbidden under TSO */
        } else if (is_wrc) {
            int r1 = atomic_load_explicit(&S->t1_a, memory_order_relaxed); /* T1 saw x  */
            int r2 = atomic_load_explicit(&S->t2_a, memory_order_relaxed); /* T2 saw y  */
            int r3 = atomic_load_explicit(&S->t2_b, memory_order_relaxed); /* T2 saw x? */
            if (r1 == 1 && r2 == 1 && r3 == 0) witnesses++;  /* non-MCA: forbidden under TSO */
        } else { /* SB */
            int r0 = atomic_load_explicit(&S->t0_a, memory_order_relaxed);
            int r1 = atomic_load_explicit(&S->t1_a, memory_order_relaxed);
            if (r0 == 0 && r1 == 0) witnesses++;    /* allowed under TSO and ARM */
        }
        total++;
    }

    pthread_join(th0, NULL);
    pthread_join(th1, NULL);
    if (is_wrc) pthread_join(th2, NULL);

    const char *tname = (g_test == TEST_MP) ? "mp" : (is_wrc ? "wrc" : "sb");
    const char *mname = g_fenced ? "fenced" : "relaxed";
    int tso_granted = atomic_load_explicit(&g_tso_granted, memory_order_relaxed);

    if (g_json) {
        printf("{\"test\":\"%s\",\"mode\":\"%s\",\"map\":\"%s\",\"iters\":%ld,\"cpu_a\":%d,"
               "\"cpu_b\":%d,\"cpu_c\":%d,\"witnesses\":%ld,\"total\":%ld,"
               "\"tso_requested\":%d,\"tso_granted\":%d}\n",
               tname, mname, map_name(g_map), g_iters, g_cpu_a, g_cpu_b,
               is_wrc ? g_cpu_c : -1, witnesses, total,
               g_request_tso, tso_granted);
    } else {
        if (is_wrc)
            printf("test=%s mode=%s map=%s iters=%ld cpu-a=%d cpu-b=%d cpu-c=%d\n",
                   tname, mname, map_name(g_map), g_iters, g_cpu_a, g_cpu_b, g_cpu_c);
        else
            printf("test=%s mode=%s map=%s iters=%ld cpu-a=%d cpu-b=%d\n",
                   tname, mname, map_name(g_map), g_iters, g_cpu_a, g_cpu_b);
        if (g_request_tso) {
            if (tso_granted == 2)
                printf("  hardware TSO: GRANTED by kernel to both threads (NO ROOT) -- "
                       "if MP witnesses are now 0, this phone can do it unrooted!\n");
            else
                printf("  hardware TSO: request DENIED (kernel has no memory-model prctl; "
                       "granted=%d/2). Measuring plain weak baseline.\n", tso_granted);
        }
        printf("  witnesses = %ld / %ld  (%.3e)\n",
               witnesses, total, total ? (double)witnesses / (double)total : 0.0);
        if (g_test == TEST_MP)
            printf("  [MP witness = r1==1 && r2==0 : forbidden under TSO, allowed under weak]\n");
        else if (is_wrc)
            printf("  [WRC witness = r1==1 && r2==1 && r3==0 : non-MCA, forbidden under TSO;"
                   " plain fires it on weak ARM (sensitivity control)]\n");
        else
            printf("  [SB witness = r0==0 && r1==0 : allowed under BOTH -> sensitivity control]\n");
    }

    free(S);
    return 0;
}
