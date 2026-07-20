# Results — empirical measurements

A lab notebook of real measurements from Project Oryx. Newest first.

---

## 2026-07-20 — Phase 0 Step 1: baseline memory model on the Galaxy S26 Ultra ✅

**First real-hardware data point. The detector works, and the baseline is confirmed WEAK.**

| Field | Value |
|-------|-------|
| Device | Samsung Galaxy S26 Ultra |
| SoC | Snapdragon 8 Elite Gen 5 (`SM8850`), Qualcomm Oryon Gen 3 |
| Method | `run_phase0.sh` via Termux, quick run (`ITERS≈2,000,000`), no root |
| Tool | `experiments/phase0-tso-probe/litmus/oryx_litmus` (MP + SB) |

### Raw output

```
pair     SB(relax)    MP(relax)    MP(fenced)   verdict
----     ---------    ---------    ----------   -------
0:1      31309        3052         0            WEAK
6:7      61           25           0            WEAK
0:6      478095       2725         0            WEAK
        (warning: could not pin to CPU 6 — Android sandbox; ran unpinned)

RESULT: WEAK model on all sensitive pairs (expected Oryon baseline).
        Harness is sensitive (SB fired).
```

### Interpretation

- **SB (store-buffer) fired on every pair** (31,309 / 61 / 478,095) → the detector is
  *sensitive* on Oryon silicon: it genuinely observes reordering. This is the validity
  control that makes the MP result trustworthy.
- **MP (message-passing) relaxed is nonzero on every pair** (3,052 / 25 / 2,725) → the
  outcome that is *forbidden under TSO* actually occurs → **the Oryon cores are weakly
  ordered** at baseline.
- **MP fenced = 0 on every pair** → inserting a real `DMB ISH` barrier eliminates the
  reordering entirely → the harness discriminates ordered vs unordered correctly, and the
  nonzero relaxed counts are real reordering, not noise.
- **Cross-cluster pair 0:6** shows by far the most reordering (SB 478k) — as expected, the
  widest store-propagation window. **Pair 6:7** (two cores, tight coherence) shows little but
  still clearly nonzero.
- The "could not pin to CPU 6" warnings are Android/Termux restricting `sched_setaffinity`
  for the app; the run proceeded unpinned and still produced a clean signal. A rooted/ADB
  run can pin properly.

### Significance

This confirms, on real hardware, the precondition the entire project rests on: **the
S26 Ultra runs in ARM's weak memory model by default.** The `MP fenced = 0` column *is* the
software fencing tax Box64/FEX currently pay — the barriers that a hardware TSO mode would
let us delete.

### Status

- **Phase 0 Step 1: PASSED.** Detector validated on target hardware; baseline = WEAK.
- **Next: Phase 0 Step 2** — the root-only control-bit hunt: find a register bit that flips
  a pair's `MP(relax)` to 0 *without* a barrier while SB stays nonzero. That would be direct
  evidence of a hardware TSO mode on Oryon — the gate for Part A. See
  [`experiments/phase0-tso-probe/probe/STEP2-RUNBOOK.md`](experiments/phase0-tso-probe/probe/STEP2-RUNBOOK.md).

### Caveats (honest scope)

Single device, single session, Termux (affinity partially unavailable). The result is
unambiguous (weak with a sensitive detector), but for the record it is one run on one unit,
not a population study.
