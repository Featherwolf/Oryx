# Part A — Light up Oryon's hardware x86 memory-ordering mode

**Goal:** make ordinary ARM loads/stores in a translated thread obey x86 TSO in *hardware*,
so Box64/FEX can stop inserting barriers — closing the ~57%→~71%-of-native gap that
separates on-device JIT from Rosetta.

**Confidence:** hardware exists **[Established]**; per-thread Android toggle **[R&D]** →
gated on [Phase 0](../experiments/phase0-tso-probe/).

## Precedent we are copying

| System | Mechanism |
|--------|-----------|
| Apple Rosetta 2 | Kernel sets a bit in `ACTLR_EL1` (IMPDEF auxiliary control register) on kernel-exit for Rosetta threads; per-thread, runtime toggle. `AIDR_EL1` bit advertises support. |
| Asahi Linux | Upstream-proposed `prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO)` / `PR_GET_MEM_MODEL` that flips the same bit per-thread. |
| Windows Prism (Oryon) | Uses Oryon's equivalent hardware x86-ordering accommodation (mechanism not public). |

Part A is: **find Oryon's equivalent of that `ACTLR_EL1` bit, and expose an Asahi-style
`prctl` for it on Android.**

## Design

### A.1 — Discovery (Phase 0 output)

Phase 0 produces: the register + bit that toggles Oryon's x86 memory-ordering mode, whether
it is per-thread and EL1-writable, and any core-type restriction (Apple's TSO threads are
pinned to performance cores). See the experiment for method.

### A.2 — Kernel surface: `PR_SET_MEM_MODEL` on Android

Mirror the ARM-Linux TSO patch series rather than inventing an interface:

```c
/* uapi: mirror of the proposed ARM-Linux interface */
#define PR_GET_MEM_MODEL   0x6d4d444c   /* 'mMDL' */
#define PR_SET_MEM_MODEL   0x6d4d444d
#define PR_MEM_MODEL_DEFAULT  0   /* ARM weak ordering */
#define PR_MEM_MODEL_TSO      1   /* x86 total store order */
```

Two delivery options, in order of preference:

1. **GKI loadable kernel module.** Samsung ships Generic Kernel Image; a signed vendor
   module can register the `prctl` handler and program the Oryon control bit on context
   switch for threads that opted in. Preferred because it needs no full kernel rebuild.
2. **Magisk module / patched boot image.** For devices where (1) is blocked, a Magisk module
   carries the same code. This is the enthusiast path (these users already flash custom
   drivers).

The module must:
- Set the bit **only** for threads that called `PR_SET_MEM_MODEL_TSO`, and restore weak
  ordering on context-switch away, so the rest of the system keeps ARM's faster weak model
  (recall TSO costs ~8.94% even in hardware — you do not want it globally).
- Honor any core-affinity restriction discovered in Phase 0 (migrate/pin TSO threads to
  eligible cores).
- Fail closed: if the bit can't be set, `prctl` returns `-EINVAL` and the caller keeps
  software ordering.

### A.3 — Emulator integration (Box64 / FEX)

On thread creation for a translated guest thread:

```
if (oryx_hw_tso_available() && prctl(PR_SET_MEM_MODEL, PR_MEM_MODEL_TSO) == 0) {
    translator.memory_model_emulation = OFF;   // emit plain LDR/STR, no LDAR/STLR/DMB
} else {
    translator.memory_model_emulation = ON;    // Lasagne-optimized fences + LSE casal
}
```

- **FEX:** flip its existing `TSOEnabled` config off for the thread (it already has the
  switch — today toggling it off is "unstable/incorrect"; with hardware TSO it becomes
  *correct*).
- **Box64:** treat as global `STRONGMEM` satisfied-in-hardware; skip the barrier codegen path.

This is the payoff: the emulators already have the "don't emulate ordering" fast path — it
is currently unsafe. Part A makes it **safe** by having the silicon provide the guarantee.

### A.4 — Fallback (Phase 0 = fail)

If Oryon's bit proves non-existent, un-toggleable, or unsafe to touch from Android:
- Ship **Lasagne-style optimized fence placement** (−45.5% fences avg) instead of the
  conservative per-access barriers.
- Translate x86 CAS to LSE `CASAL` (helps under low contention).
- This is strictly the software floor — real, but not the Rosetta tier.

## Risks & mitigations

| Risk | Mitigation |
|------|-----------|
| Oryon's mode is all-or-nothing / always-on-only | Then per-process opt-in is impossible; measure whether a *whole-container* TSO mode is viable instead |
| Setting the bit destabilizes the SoC | Extensive litmus + soak testing before exposing; fail-closed default |
| Samsung locks GKI module loading | Fall back to Magisk path; pursue OEM/Qualcomm cooperation (Phase 3) |
| Core-affinity limits parallelism | Accept TSO threads on eligible cores only; schedule non-TSO work on the rest |
| Security: arbitrary apps flipping memory model | `prctl` gated behind Oryx's signed module + per-app entitlement |

## Definition of done

- `prctl(PR_SET_MEM_MODEL, TSO)` succeeds on an S26 Ultra and the MP litmus test
  (Phase 0) reports **zero** reordering with it on, nonzero with it off.
- A multi-threaded x86 game that crashes under Box64 without `STRONGMEM` runs stably with
  ordering emulation **off** and hardware TSO **on**.
- Measured CPU-bound uplift toward the Rosetta tier on a representative title.
