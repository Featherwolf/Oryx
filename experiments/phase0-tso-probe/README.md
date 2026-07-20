# Phase 0 — The crux experiment

**The single question that gates Project Oryx:**

> Can the Oryon Gen 3 cores in the Galaxy S26 Ultra be switched into an x86-style
> **Total Store Order** memory mode, per-thread, from software we can ship on Android?

Part A — the headline performance bet — is worthless if the answer is no. So we answer it
first, cheaply, before building anything else. Everything downstream branches on the result.

## What we know going in

- Oryon **has** hardware x86 memory-ordering accommodations; Windows Prism uses them.
  **[Established]**
- A per-thread, runtime-toggleable hardware TSO mode is documented for Apple (a bit in
  `ACTLR_EL1`) and confirmed always-on for NVIDIA Carmel / Fujitsu A64FX. **Qualcomm is not
  named** in the toggle-mechanism literature. **[Established, as a negative]**
- So the *mechanism* on Oryon is unknown to us. That is exactly the gap this experiment
  closes.

## Two tools, one decision

```
experiments/phase0-tso-probe/
  litmus/      A memory-model detector (userspace, no root).
               oryx_litmus.c  — MP + SB litmus tests (see litmus/README.md)
               run_phase0.sh  — automated protocol + per-pair verdict
               Empirically measures whether the effective memory model
               is weak (reordering observed) or TSO (reordering forbidden).
  probe/       A kernel-module method to (a) find Oryon's memory-mode
               control bit and (b) toggle it per-thread, then re-run litmus.
```

### Step 1 — Validate the detector (no root, safe on any device)

Build and run `litmus/` on the S26 Ultra. `run_phase0.sh` executes the full matrix; the
individual signals it combines are:

| Test / mode | What it does | Expected on stock ARM | Proves |
|-------------|--------------|-----------------------|--------|
| `sb relaxed` | Store-Buffer test (allowed under TSO *and* weak) | **nonzero** | the detector is sensitive on this silicon |
| `mp relaxed` | Message-Passing test, no barriers | **nonzero** (weak) | the pair is weakly ordered at baseline |
| `mp fenced` | Same, but with a `DMB ISH` barrier | **zero** | the detector can *discriminate* ordered vs unordered |

The Store-Buffer test is the sensitivity control: if it shows zero, the harness isn't
exposing reordering (raise `ITERS`, use cross-cluster core pairs) and any MP result is
meaningless. **A meaningful TSO result requires SB firing first.** See
[`litmus/README.md`](litmus/README.md) for the full decision rule.

### Step 2 — Find and flip the bit (needs root / unlocked bootloader)

In `probe/`, scan Oryon's IMPLEMENTATION DEFINED system registers (the `S3_<op1>_C15_*`
`ACTLR`/`AIDR`-class space) for the control that changes the litmus outcome. Method:

1. From EL1 (kernel module), read the candidate IMPDEF registers; diff against the
   documented Apple layout and any Prism-enabled reference.
2. For each candidate bit, set it for the current thread, re-run the litmus kernel, and
   watch for the `relaxed` reordering count to drop to **zero**.
3. The bit whose setting turns `relaxed` into `zero` **without** a barrier is Oryon's TSO
   toggle.

### Step 3 — The decision

| Litmus result with candidate bit set | Verdict | Consequence |
|--------------------------------------|---------|-------------|
| `relaxed` → **zero**, per-thread, stable | **PASS** | Build Part A: `PR_SET_MEM_MODEL` + emulator integration |
| No bit changes the outcome | **FAIL** | Fall back to Lasagne-optimized fences + LSE `casal` |
| Bit works but is **global / all-or-nothing** | **PARTIAL** | Evaluate whole-container TSO instead of per-thread |
| Bit works but destabilizes SoC | **FAIL-SAFE** | Do not ship; document and stop Part A |

## Safety

- Step 1 is ordinary userspace — safe anywhere.
- Step 2 pokes IMPDEF EL1 registers and **can crash or brick a device**. Run only on a
  dedicated, bootloader-unlocked test unit with a recovery path. Never on a daily driver.
- The probe module fails closed and restores registers on unload.

## Why this is the right first move

It is cheap (days, not months), decisive (a single binary yes/no), and it de-risks the
entire project before a line of Part A production code is written. If it fails, we learn
early and ship the honest software floor instead — no sunk cost.
