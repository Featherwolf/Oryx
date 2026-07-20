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

#define CACHELINE 128

/* Each shared variable sits alone on a cache line to widen the reordering window. */
typedef struct {
    _Alignas(CACHELINE) atomic_int x;
    _Alignas(CACHELINE) atomic_int y;
    _Alignas(CACHELINE) atomic_int t0_a;   /* T0's observed load (SB) */
    atomic_int t0_b;
    _Alignas(CACHELINE) atomic_int t1_a;   /* T1's observed load(s) (MP/SB) */
    atomic_int t1_b;
    _Alignas(CACHELINE) atomic_long gate_go;
    _Alignas(CACHELINE) atomic_long t0_done;
    _Alignas(CACHELINE) atomic_long t1_done;
} shared_t;

enum test_kind { TEST_MP, TEST_SB };

static shared_t *S;
static long g_iters  = 5000000L;   /* mobile-friendly default; raise for more sensitivity */
static int  g_fenced = 0;
static int  g_cpu_a  = 0;
static int  g_cpu_b  = 1;
static int  g_json   = 0;
static enum test_kind g_test = TEST_MP;

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
    for (long i = 1; i <= g_iters; i++) {
        wait_gate(&S->gate_go, i);
        if (g_test == TEST_MP) {
            atomic_store_explicit(&S->x, 1, memory_order_relaxed);
            if (g_fenced) barrier_full();
            atomic_store_explicit(&S->y, 1, memory_order_relaxed);
        } else { /* SB */
            atomic_store_explicit(&S->x, 1, memory_order_relaxed);
            if (g_fenced) barrier_full();
            int r0 = atomic_load_explicit(&S->y, memory_order_relaxed);
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
    for (long i = 1; i <= g_iters; i++) {
        wait_gate(&S->gate_go, i);
        if (g_test == TEST_MP) {
            int r1 = atomic_load_explicit(&S->y, memory_order_relaxed);
            if (g_fenced) barrier_full();
            int r2 = atomic_load_explicit(&S->x, memory_order_relaxed);
            atomic_store_explicit(&S->t1_a, r1, memory_order_relaxed);
            atomic_store_explicit(&S->t1_b, r2, memory_order_relaxed);
        } else { /* SB */
            atomic_store_explicit(&S->y, 1, memory_order_relaxed);
            if (g_fenced) barrier_full();
            int r1 = atomic_load_explicit(&S->x, memory_order_relaxed);
            atomic_store_explicit(&S->t1_a, r1, memory_order_relaxed);
        }
        atomic_store_explicit(&S->t1_done, i, memory_order_release);
    }
    return NULL;
}

static void parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--test") && i + 1 < argc) {
            const char *t = argv[++i];
            if      (!strcmp(t, "mp")) g_test = TEST_MP;
            else if (!strcmp(t, "sb")) g_test = TEST_SB;
            else { fprintf(stderr, "unknown --test %s (mp|sb)\n", t); exit(2); }
        } else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            g_fenced = !strcmp(argv[++i], "fenced");
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            g_iters = strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--cpu-a") && i + 1 < argc) {
            g_cpu_a = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--cpu-b") && i + 1 < argc) {
            g_cpu_b = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--json")) {
            g_json = 1;
        } else {
            fprintf(stderr,
                "usage: %s [--test mp|sb] [--mode relaxed|fenced] [--iters N] "
                "[--cpu-a A] [--cpu-b B] [--json]\n", argv[0]);
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

    pthread_t th0, th1;
    if (pthread_create(&th0, NULL, thread0, NULL) ||
        pthread_create(&th1, NULL, thread1, NULL)) {
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

        if (g_test == TEST_MP) {
            int r1 = atomic_load_explicit(&S->t1_a, memory_order_relaxed);
            int r2 = atomic_load_explicit(&S->t1_b, memory_order_relaxed);
            if (r1 == 1 && r2 == 0) witnesses++;   /* forbidden under TSO */
        } else { /* SB */
            int r0 = atomic_load_explicit(&S->t0_a, memory_order_relaxed);
            int r1 = atomic_load_explicit(&S->t1_a, memory_order_relaxed);
            if (r0 == 0 && r1 == 0) witnesses++;    /* allowed under TSO and ARM */
        }
        total++;
    }

    pthread_join(th0, NULL);
    pthread_join(th1, NULL);

    const char *tname = (g_test == TEST_MP) ? "mp" : "sb";
    const char *mname = g_fenced ? "fenced" : "relaxed";

    if (g_json) {
        printf("{\"test\":\"%s\",\"mode\":\"%s\",\"iters\":%ld,\"cpu_a\":%d,"
               "\"cpu_b\":%d,\"witnesses\":%ld,\"total\":%ld}\n",
               tname, mname, g_iters, g_cpu_a, g_cpu_b, witnesses, total);
    } else {
        printf("test=%s mode=%s iters=%ld cpu-a=%d cpu-b=%d\n",
               tname, mname, g_iters, g_cpu_a, g_cpu_b);
        printf("  witnesses = %ld / %ld  (%.3e)\n",
               witnesses, total, total ? (double)witnesses / (double)total : 0.0);
        if (g_test == TEST_MP)
            printf("  [MP witness = r1==1 && r2==0 : forbidden under TSO, allowed under weak]\n");
        else
            printf("  [SB witness = r0==0 && r1==0 : allowed under BOTH -> sensitivity control]\n");
    }

    free(S);
    return 0;
}
