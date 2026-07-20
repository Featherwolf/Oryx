# No-root correctness validation for the memory-model translator

The DRF translator's whole premise is *removing* ordering. If the classifier ever calls a
SHARED access LOCAL, the result is **silent memory corruption** — the worst kind of bug, rare
and unreproducible. And on a stock phone we have **no hardware-TSO reference** to diff against.
So validation is not optional, and it must be **no-root**. This document is the plan.

Guiding asymmetry: misclassifying SHARED→LOCAL is a correctness bug; LOCAL→SHARED only costs
speed. So the harness is tuned to **catch lost ordering**, not lost performance.

## Three layers, all no-root

### Layer A — on-device litmus battery (`litmus7`, userspace, no root)

herdtools **`litmus7`** runs entirely in userspace and is documented to run on Android — it JITs
a stress harness around a short concurrent assembly snippet and reports observed outcomes.
(`klitmus7`, the kernel-module variant, needs root → **out**.) It can't *prove* a model, but it
can *disprove* one: run enough iterations and a permitted-by-your-mapping-but-forbidden-by-TSO
outcome will eventually appear.

**The battery and their x86-TSO verdicts:**

| Test | Probes | TSO verdict |
|------|--------|-------------|
| **MP** (message passing) | store→store / load→load ordering | **forbidden** |
| **SB** (store buffer) | store→load (the one relaxation) | **allowed** ← must be observed |
| **LB** (load buffer) | load→store ordering | **forbidden** |
| **2+2W** | store→store across two locations | **forbidden** |
| **WRC** (write-to-read causality) | **multi-copy atomicity** (3 threads) | **forbidden** |
| **IRIW** (independent reads of independent writes) | **multi-copy atomicity** (4 threads) | **forbidden** |

Two assertions per run: (1) the TSO-**forbidden** outcome is **never** observed; (2) the SB
(TSO-**allowed**) outcome **is** observed — otherwise the mapping is accidentally *over-strong*
(e.g. full SC), which would mask a wrong-direction bug and give false confidence.

**Mapping-under-test methodology (the key idea).** Standard litmus7 measures the bare hardware.
To validate a *mapping*, write each test's memory ops using **the mapping's actual instructions**
and run all variants:

| Mapping variant | Load / Store lowering | WRC/IRIW (forbidden) | SB (TSO-**allowed**) |
|-----------------|-----------------------|----------------------|----------------------|
| Exact-TSO RCpc  | `LDAPR` / `STLR` | forbidden holds ✅ | **observed ✅** |
| Exact-TSO fence | `LDR;DMB ISHLD` / `DMB ISHST;STR` | forbidden holds ✅ | **observed ✅** |
| Conservative SC | `LDAR` / `STLR` | forbidden holds ✅ | *not* observed (over-strong) |

**All three mappings keep WRC and IRIW forbidden** — ARMv8 is *other*-multi-copy-atomic, and none
of these primitives break that. The distinguishing test is **`SB`, not WRC/IRIW.** `SB` (the
store-buffer / W→R outcome) is **allowed by x86-TSO**, so the two *exact-TSO* mappings
(`LDAPR`/`STLR` and the DMB scheme) must **exhibit** it, while the *over-strong* SC mapping
(`LDAR`/`STLR`) forbids it. Observing `SB` under `LDAPR`/`STLR` is what proves the mapping is
exact-TSO and not accidentally sequentially consistent — the presence of an allowed outcome has
teeth, just like the absence of a forbidden one.

> An earlier draft of this table predicted `LDAPR`/`STLR` would *violate* WRC/IRIW and called it
> "unsound." That was wrong: `LDAPR`/`STLR` is the exact minimal TSO mapping (it is what FEX
> ships), keeps WRC/IRIW forbidden, and differs from SC only on `SB` — a TSO-allowed outcome. See
> `docs/box64-fex-integration.md` §4 for the axiomatic argument.

`liboryxtu` emits all three variants (`ORYX_ORDER_RCPC` = `LDAPR`/`STLR`, exact-TSO;
`ORYX_ORDER_SC` = `LDAR`/`STLR`, conservative; `ORYX_ORDER_TSO` = the DMB-fence scheme), so the
litmus snippets can be generated from the same lowering the translator uses — keeping test and
product in lockstep.

### Layer B — differential testing (weak vs conservative, same inputs)

Run every workload twice on-device: once under the **DRF/weak** translation, once under the
**conservative** (`LDAR`/full-DMB, `STRONGMEM`-max) translation, with identical inputs and RNG
seeds. Any divergence in observable output flags an access the DRF classifier wrongly relaxed.
Pair with a tsan-style race detector and record/replay for reproduction. This catches
classifier bugs that litmus (which tests the *primitives*, not the *classification*) cannot.

### Layer C — offline, device-independent (highest assurance)

- Encode each mapped instruction sequence as a **herd7 `.cat`** model check — decides
  forbidden/allowed against the formal AArch64 model, no device needed.
- Adopt/port a **machine-checked** mapping (Lasagne's Agda, Arancini's proofs) so correctness of
  the *primitive* mapping never rests on any single device's observed behavior. Layers A/B then
  only need to validate the *classifier*, not the mapping.

## What exists toward this today

- **Layer A mapping validator — runnable, no root** (`experiments/phase0-tso-probe/litmus/`, the
  `--map {plain,rcpc,sc,dmb}` mode + `run_layerA.sh`): lowers the 2-thread MP and SB tests using
  the translator's actual `LDAPR`/`STLR`, `LDAR`/`STLR`, and DMB-fence sequences and asserts (1)
  MP-forbidden is 0 for all three mappings and (2) SB fires under `rcpc`/`dmb` but not `sc`. The
  aarch64 build emits the real `ldapr`/`ldar`/`stlr`/`dmb ish{ld,st}` instructions (verified by
  cross-compile disassembly); the host build falls back to matching C11 atomics for CI/self-test.
  This is the on-device proof of the exact-TSO mapping, in one no-root binary — **no `litmus7`
  install needed** for the 2-thread cases. The `SB` result is the *complete* `rcpc`-vs-`sc` test
  (see the "distinguishing test is `SB`, not WRC/IRIW" note above), so it clears `rcpc` as exact-TSO.
- **Hardware-MCA sanity check** (same binary, `--test wrc` + `run_mca.sh`): a 3-thread WRC test that
  confirms this silicon keeps the non-multi-copy-atomic outcome forbidden under every *ordered*
  mapping. It is **not** an `rcpc` gate — `rcpc` and `sc` are identical on WRC (neither has the
  `STLR`→`LDAPR` edge RCpc relaxes) — it checks the hardware other-MCA property both mappings rely on.
- **Phase 0 hardware detector** (same binary, `--map plain` + `run_phase0.sh`): the bare-hardware
  MP + SB baseline, validated on a real S26 Ultra (baseline = WEAK, SB sensitive).
- **`liboryxtu` ordering strengths**: `ORYX_ORDER_RCPC` (`LDAPR`/`STLR`, exact-TSO), `ORYX_ORDER_SC`
  (`LDAR`/`STLR`), and `ORYX_ORDER_TSO` (DMB-fence) produce all three sound mappings' exact bytes
  (known-answer tested), matching what the litmus validator emits — test and product in lockstep.
- **The corrected mapping table** (`docs/box64-fex-integration.md` §4) defines the ground-truth
  each variant is checked against.

## Plan

1. **Run Layer A on-device** (done tooling; `sh run_layerA.sh`): confirm ordering restored by all
   three mappings and the `rcpc`-fires / `sc`-suppresses `SB` exactness signature on Oryon. Because
   `SB` is the sole `rcpc`-vs-`sc` discriminator, this **clears `rcpc` as exact-TSO** — no root
   needed. *(Validated on a real S26 Ultra: MP 35→0 under all mappings; `rcpc` SB=5680, `sc` SB=0.)*
2. **Hardware-MCA sanity** (optional; `sh run_mca.sh`, or `litmus7` for higher WRC/IRIW yield):
   confirm every *ordered* mapping keeps WRC/IRIW forbidden, i.e. the silicon is multi-copy-atomic.
   This is a property `rcpc` and `sc` rely on **equally** — it is not a separate `rcpc` gate (they
   don't differ on WRC/IRIW), which adversarial review confirmed. `litmus7` is preferred here since
   the hand-rolled lockstep WRC has low yield.
3. **Stand up differential testing**: a harness that runs a guest workload under weak vs
   conservative translation and diffs outputs, with a seed/affinity/shuffle stress config tuned
   for big.LITTLE.
4. **Add offline `herd7` checks** to CI for each mapping variant so a mapping regression fails
   the build before it reaches a device.

## Honest limits

- litmus7 **disproves, never proves** — a clean battery is strong evidence, not a guarantee;
  Layer C (formal) is what closes that gap for the primitive mapping.
- The classifier's soundness (Layer B territory) is only as good as its escape analysis; litmus
  can't cover it, which is why differential testing is mandatory, not optional.
- Best on-device stress parameters (affinity, memory shuffling, preload) to surface the rare
  W→R and multi-copy-atomicity outcomes within an app sandbox need tuning per SoC.
