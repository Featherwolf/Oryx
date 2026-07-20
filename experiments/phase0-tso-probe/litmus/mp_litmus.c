/*
 * mp_litmus.c — Message-Passing memory-model detector for Project Oryx, Phase 0.
 *
 * Purpose
 * -------
 * Empirically decide whether the *effective* memory model on this core pair is
 * weakly ordered (ARM default) or Total Store Order (x86 / Apple-TSO / Oryon-TSO).
 *
 * The classic Message-Passing (MP) litmus test:
 *
 *      writer (T0)            reader (T1)
 *      ----------            -----------
 *      x = 1                 r1 = y
 *      y = 1                 r2 = x
 *
 * The outcome (r1 == 1 && r2 == 0) — "saw the flag but not the data" — is:
 *      - ALLOWED under ARM's weak model (store-store or load-load reordering)
 *      - FORBIDDEN under x86 TSO
 *
 * So: count that outcome over many trials.
 *      relaxed mode, weak core   -> nonzero  (reordering happens)
 *      relaxed mode, TSO core    -> zero     (reordering forbidden by hardware)
 *      fenced  mode, any core    -> zero     (barrier restores order; validates the harness)
 *
 * IMPORTANT: a zero count only *means* TSO once you have shown the harness produces a
 * nonzero count on this silicon in relaxed/weak mode. Otherwise the test simply lacks
 * sensitivity. Run --mode relaxed on a stock (weak) device FIRST and confirm nonzero.
 *
 * Build: see Makefile (targets an aarch64 Android/Linux device).
 * Usage: ./mp_litmus [--mode relaxed|fenced] [--iters N] [--cpu-a A] [--cpu-b B]
 */

#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>

/* Put the two variables on separate cache lines to widen the reordering window. */
#define CACHELINE 128
typedef struct {
    _Alignas(CACHELINE) atomic_int x;
    _Alignas(CACHELINE) atomic_int y;
    /* Reader's observed values, published back for the main thread to score. */
    _Alignas(CACHELINE) atomic_int r1;
    atomic_int r2;
    /* Sense-reversing barrier state to align the two threads each trial. */
    _Alignas(CACHELINE) atomic_int gate_go;    /* incremented by main to release a trial */
    atomic_int gate_writer_done;
    atomic_int gate_reader_done;
} shared_t;

static shared_t *S;
static long   g_iters   = 20000000L;   /* 20M trials by default */
static int    g_fenced  = 0;           /* 0 = relaxed, 1 = insert DMB ISH */
static int    g_cpu_a   = 0;
static int    g_cpu_b   = 1;

static void pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        fprintf(stderr, "warning: could not pin to CPU %d (continuing unpinned)\n", cpu);
}

/* A full system barrier — the "fenced" control that should force zero reordering.
 * On aarch64 (the real target) emit the architected DMB ISH; elsewhere (dev host)
 * fall back to a C11 seq-cst fence so the harness still builds and self-tests. */
static inline void dmb_ish(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dmb ish" ::: "memory");
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

/* Spin until the trial counter reaches `target`. */
static inline void wait_gate(atomic_int *gate, int target)
{
    while (atomic_load_explicit(gate, memory_order_acquire) < target)
        /* busy-wait */;
}

static void *writer_thread(void *arg)
{
    (void)arg;
    pin_to_cpu(g_cpu_a);
    for (long i = 1; i <= g_iters; i++) {
        wait_gate(&S->gate_go, i);
        /* The two stores under test — relaxed so hardware ordering shows through. */
        atomic_store_explicit(&S->x, 1, memory_order_relaxed);
        if (g_fenced) dmb_ish();
        atomic_store_explicit(&S->y, 1, memory_order_relaxed);
        atomic_store_explicit(&S->gate_writer_done, i, memory_order_release);
    }
    return NULL;
}

static void *reader_thread(void *arg)
{
    (void)arg;
    pin_to_cpu(g_cpu_b);
    for (long i = 1; i <= g_iters; i++) {
        wait_gate(&S->gate_go, i);
        int r1 = atomic_load_explicit(&S->y, memory_order_relaxed);
        if (g_fenced) dmb_ish();
        int r2 = atomic_load_explicit(&S->x, memory_order_relaxed);
        atomic_store_explicit(&S->r1, r1, memory_order_relaxed);
        atomic_store_explicit(&S->r2, r2, memory_order_relaxed);
        atomic_store_explicit(&S->gate_reader_done, i, memory_order_release);
    }
    return NULL;
}

static void parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            g_fenced = !strcmp(argv[++i], "fenced");
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            g_iters = strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--cpu-a") && i + 1 < argc) {
            g_cpu_a = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--cpu-b") && i + 1 < argc) {
            g_cpu_b = atoi(argv[++i]);
        } else {
            fprintf(stderr, "usage: %s [--mode relaxed|fenced] [--iters N] "
                            "[--cpu-a A] [--cpu-b B]\n", argv[0]);
            exit(2);
        }
    }
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);

    S = aligned_alloc(CACHELINE, sizeof(shared_t));
    if (!S) { perror("aligned_alloc"); return 1; }
    memset(S, 0, sizeof(*S));

    pthread_t tw, tr;
    pthread_create(&tw, NULL, writer_thread, NULL);
    pthread_create(&tr, NULL, reader_thread, NULL);

    long reordered = 0;   /* r1==1 && r2==0  -> weak-model reordering observed */
    long both_seen = 0;   /* r1==1 && r2==1  -> ordered / propagated */
    long none_seen = 0;   /* r1==0           -> flag not yet visible; inconclusive trial */

    for (long i = 1; i <= g_iters; i++) {
        /* Reset the variables for this trial. */
        atomic_store_explicit(&S->x, 0, memory_order_relaxed);
        atomic_store_explicit(&S->y, 0, memory_order_relaxed);

        /* Release both threads for trial i. */
        atomic_store_explicit(&S->gate_go, i, memory_order_release);

        /* Wait for both to finish trial i. */
        wait_gate(&S->gate_writer_done, i);
        wait_gate(&S->gate_reader_done, i);

        int r1 = atomic_load_explicit(&S->r1, memory_order_relaxed);
        int r2 = atomic_load_explicit(&S->r2, memory_order_relaxed);
        if (r1 == 1 && r2 == 0)      reordered++;
        else if (r1 == 1 && r2 == 1) both_seen++;
        else                         none_seen++;
    }

    pthread_join(tw, NULL);
    pthread_join(tr, NULL);

    printf("mode=%s iters=%ld cpu-a=%d cpu-b=%d\n",
           g_fenced ? "fenced" : "relaxed", g_iters, g_cpu_a, g_cpu_b);
    printf("  reordered (r1=1,r2=0)  = %ld   <-- weak-model witnesses\n", reordered);
    printf("  both_seen (r1=1,r2=1)  = %ld\n", both_seen);
    printf("  inconclusive (r1=0)    = %ld\n", none_seen);
    printf("\n");
    if (reordered > 0)
        printf("VERDICT: reordering OBSERVED -> effective model is WEAK on this pair.\n");
    else
        printf("VERDICT: no reordering observed. Either TSO is in effect, OR the test "
               "lacked sensitivity (confirm relaxed/weak shows nonzero first!).\n");

    free(S);
    return 0;
}
