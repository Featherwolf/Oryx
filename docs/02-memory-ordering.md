# 02 — Memory Ordering: the core problem

This is the single most important technical document in the project. Everything about
Part A follows from it.

## x86 is strong, ARM is weak

- **x86-64** implements **Total Store Order (TSO)**: the only reordering a core may observe
  is store→load (a later load bypassing an earlier store to a *different* address). Loads
  are never reordered with loads; stores never with stores; a load never bypasses an
  earlier store to the *same* address.
- **ARM (AArch64)** is **weakly ordered**: absent barriers or acquire/release semantics,
  loads and stores to different addresses may be observed in almost any order by another
  core.

A program written and tested only on x86 can rely on TSO *implicitly* — its correctness may
depend on orderings x86 guarantees for free. Run that same binary's instructions naively on
weak ARM and those orderings evaporate, surfacing as data races, corrupted state, and
crashes — especially in multi-threaded games. Google's warehouse-scale x86→ARM migration
found "Memory Model & Concurrency Adjustments" to be a distinct, required category of source
fixes; *"a data race may be hidden by x86's TSO memory model."* **[Established — arXiv 2510.14928]**

## How a translator must handle it (in software)

To be **correct**, a dynamic binary translator emulating x86 on weak ARM must restore the
lost orderings by turning guest memory ops into ordered ones:

```
x86 load   →  ARM load-acquire  (LDAR / LDAPR)     or a plain load + DMB
x86 store  →  ARM store-release (STLR)             or a DMB + plain store
x86 CAS    →  ARM CASAL (LSE)   or LL/SC loop
```

Those extra barriers/ordered-ops are the tax. The evidence on how heavy it is:

| Source | Finding | Confidence |
|--------|---------|-----------|
| FEX-Emu docs | Emulating the x86 memory model is *"the number one most costly feature"*; up to **~10×** on some vector/memcpy ops. Ships toggles to skip it (fast but *"incorrect emulation"*). | [Established] |
| Box64 | No automatic ordering; exposes `BOX64_DYNAREC_STRONGMEM` to be raised **per game** to stop multi-threaded crashes. | [Established] |
| Lasagne (PLDI'22) | Formally-proved x86↔ARM fence mappings; optimizations cut fence count by **45.5% avg / ~65% max** — but fences remain. | [Established] |
| Risotto (ASPLOS'23) | QEMU *over*-enforces ordering **and** still has fence bugs; correct+optimized placement buys only **~6.7% avg**. LSE `casal` helps CAS **only under low contention**. | [Established] |
| TOSTING (ARCS'23) | Even Apple's **hardware** TSO costs **~8.94%** vs weak ordering (SPEC FP); worst case >2× on store-heavy workloads. | [Established] |

**The lesson:** software fences are a floor you can *lower but never remove*. The only way to
pay ~0% instead of "lowered-but-nonzero" is to make the hardware enforce ordering — which is
exactly what Apple/Prism do, and what Part A proposes for Oryon.

## The hardware alternative (Part A)

Apple's cores expose a per-thread hardware TSO mode: the kernel sets a bit in `ACTLR_EL1`
on kernel-exit for Rosetta threads, after which ordinary loads/stores obey x86 TSO with **no
inserted barriers**. Rosetta translates memory ops as plain loads/stores and lets the silicon
do the ordering. That is the bulk of the Rosetta-vs-Box64 speed gap. **[Established]**

Oryon has the equivalent hardware. The open questions Phase 0 must answer:

1. **Which register/bit** controls Oryon's x86 memory-ordering mode? (Reverse-engineer;
   compare against the Prism-enabled Windows configuration.)
2. **Is it per-thread and runtime-toggleable**, or all-or-nothing? Apple's is per-thread;
   always-on vendors (NVIDIA/Fujitsu) can't toggle. Which is Oryon?
3. **Can an Android kernel module set it** safely for a translated process's threads without
   destabilizing the rest of the system?

If (1)–(3) are yes, Box64/FEX drop their ordering emulation for translated threads and the
Android stack inherits Rosetta's core advantage. If not, we fall back to Lasagne-style
optimized fences + LSE `casal` as the best software floor.

## Why this is worth the reverse-engineering risk

Every other lever in the literature (Lasagne, Risotto) moves the software floor by single-
to-double-digit percentages. The hardware mode is the difference between the ~57% (Box64)
and ~71% (Rosetta) native-speed tiers on identical silicon. On a chip that already contains
the feature, not using it is the largest unforced performance loss in the entire stack.
