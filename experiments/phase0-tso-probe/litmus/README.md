# Phase 0 litmus harness

A memory-model detector: it decides empirically whether a given pair of cores behaves as
**weakly ordered** (ARM default) or **Total Store Order** (x86 / Apple-TSO / a hypothetical
Oryon-TSO mode). This is Step 1 of [Phase 0](../README.md) and the yardstick Step 2 (the
kernel probe) uses to confirm it found the right bit.

## Build & run

```sh
make                              # native build (e.g. on-device under Termux)
make NDK=/path/to/ndk API=34      # cross-build with the Android NDK
make selftest                     # sanity-check the binary on the build host

# on the device:
adb push oryx_litmus run_phase0.sh /data/local/tmp/
adb shell "cd /data/local/tmp && sh run_phase0.sh"
```

## Two roles: hardware detector (Phase 0) and mapping validator (Layer A)

The same binary does two jobs, selected by the `--map` flag:

- **Phase 0 (`run_phase0.sh`, default `--map plain`):** measures the *bare hardware* memory
  model on a core pair (weak vs TSO). This is Step 1 of Phase 0.
- **Layer A (`run_layerA.sh`):** measures the *software mapping*. Each shared litmus access is
  lowered with the exact instructions the translator emits for a SHARED access, and the outcome
  validates that the mapping is correct — and that `LDAPR`/`STLR` is *exact* TSO. See
  `../../../docs/validation-harness.md` Layer A. Run it with:

  ```sh
  sh run_layerA.sh                 # plain/rcpc/sc/dmb x MP/SB, prints the verdict
  ITERS=20000000 sh run_layerA.sh  # raise if INCONCLUSIVE
  PAIR="2:7" sh run_layerA.sh      # force a cross-cluster pair
  ```

  `--map` lowers the two shared accesses as:

  | `--map` | store | load | property |
  |---------|-------|------|----------|
  | `plain` | `STR` | `LDR` | weak (untranslated baseline) |
  | `rcpc`  | `STLR` | `LDAPR` | **exact x86-TSO** (needs FEAT_LRCPC, ARMv8.3) |
  | `sc`    | `STLR` | `LDAR` | RCsc — stronger than TSO (over-strong) |
  | `dmb`   | `DMB ISHST; STR` | `LDR; DMB ISHLD` | exact x86-TSO via fences (pre-8.3) |

  Expected on Oryon: MP-forbidden is **0** for `rcpc`/`sc`/`dmb` (ordering restored), while
  SB-allowed is **>0** for `rcpc`/`dmb` but **0** for `sc` (proving `rcpc` is exact-TSO and `sc`
  is over-strong). On a non-aarch64 build host the primitives fall back to C11 atomics so the
  tool still builds and self-tests; the `rcpc`-vs-`sc` SB contrast only appears on real ARM.

## The two tests, and why both are needed

| Test | Threads | Witness outcome | Meaning |
|------|---------|-----------------|---------|
| **MP** (Message Passing) | `T0: x=1; y=1` / `T1: r1=y; r2=x` | `r1==1 && r2==0` | **Forbidden under TSO**, allowed under weak → *distinguishes the models* |
| **SB** (Store Buffer) | `T0: x=1; r0=y` / `T1: y=1; r1=x` | `r0==0 && r1==0` | Allowed under **both** → *sensitivity control* |

The SB test is the crucial rigor: it reorders via the store buffer, which **every** modern
out-of-order core does — including x86/TSO cores. So a nonzero SB proves the harness is
actually exposing reordering on this core pair. Only then does a zero-MP result mean "TSO"
rather than "the test wasn't sensitive."

## Decision rule (implemented in `run_phase0.sh`)

| SB (relaxed) | MP (relaxed) | MP (fenced) | Verdict |
|:---:|:---:|:---:|---|
| > 0 | > 0 | 0 | **WEAK** — expected Oryon baseline |
| > 0 | 0 | 0 | **TSO** — strong ordering in effect on this pair |
| 0 | — | 0 | **INCONCLUSIVE** — insensitive; raise `ITERS`, change pair |
| — | — | ≠ 0 | **HARNESS-BUG** — a real barrier must forbid the MP outcome |

## Validated behavior

Running `make selftest` on an x86 host (which *is* a TSO machine) produces the textbook
result and proves the tool discriminates correctly:

```
SB relaxed : witnesses > 0      (store buffer visible even on TSO — harness is sensitive)
MP relaxed : witnesses = 0      (x86 is TSO — the forbidden outcome never occurs)
MP fenced  : witnesses = 0      (barrier restores order)
=> verdict TSO   (correct: x86 is Total Store Order)
```

On an Oryon S26 Ultra with **no** TSO bit set, MP(relaxed) should instead be **nonzero**
(→ WEAK). The whole point of Step 2 is to find a control bit that flips that pair's verdict
from WEAK to TSO — that is the signature of Oryon's Rosetta-style memory mode.

## How to use it in the two steps

1. **Baseline (no root):** run `run_phase0.sh`. Expect **WEAK** everywhere, with SB firing.
   If SB doesn't fire, the run is inconclusive — tune `ITERS`/pairs until it does before
   trusting any MP result.
2. **Probe (root, Step 2):** the kernel module sets a candidate bit, then re-runs the same
   protocol. A pair flipping **WEAK → TSO** (SB still firing) identifies the bit.

## Tuning sensitivity

If SB witnesses are low or zero on the device:
- Increase `ITERS` (default 5M).
- Prefer **cross-cluster** pairs (one Prime + one Performance core) — different clusters
  widen the store-propagation window.
- Reduce lockstep: the per-trial gate keeps threads aligned; if it over-synchronizes, a
  future variant can batch K stores per gate release.
