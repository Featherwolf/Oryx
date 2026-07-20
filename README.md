# Project Oryx

[![CI](https://github.com/Featherwolf/Oryx/actions/workflows/ci.yml/badge.svg)](https://github.com/Featherwolf/Oryx/actions/workflows/ci.yml)

**Rosetta-class x86-on-ARM for the Samsung Galaxy S26 Ultra.**

Oryx is not a new emulator. It is a thin layer that slots **under** existing Android
PC-gaming front-ends (Winlator / GameNative / GameHub) and closes the gap between
their current x86 translation speed and what the hardware is actually capable of.

## The one-sentence bet

> Stop emulating x86's memory model in software. Switch the S26 Ultra's Qualcomm Oryon
> cores into the x86 memory-ordering mode they **already ship with** — then never
> translate the same code block or compile the same shader twice.

The Snapdragon 8 Elite Gen 5 (`SM8850`, Oryon Gen 3) was co-designed for Windows-on-ARM,
so it carries the same two hardware accommodations that make Apple's Rosetta 2 fast:
a hardware **Total Store Order (TSO)** memory mode and an x86/x87 floating-point behavior
mode. Microsoft's Prism emulator uses them. The Android stack (Box64/FEX + Wine + DXVK →
Vulkan → Turnip) does not — so it pays a software fencing tax that Rosetta simply doesn't.

See [`docs/00-architecture.md`](docs/00-architecture.md) for the full rationale and the
evidence behind every load-bearing claim.

## The four parts

| Part | What | Attacks | Risk |
|------|------|---------|------|
| **A** | Light up Oryon's hardware x86 memory-ordering mode | The software fencing tax (FEX's #1 cost; Box64 crashes) | Needs reverse-engineering + kernel privilege |
| **B** | Portable, content-addressed translation & shader cache ("translation CDN") | Cold-start JIT stutter & shader-compile hitching | Deterministic codegen |
| **C** | Crowd-sourced auto-tuning profiles | Per-game manual tuning hell | Low — pure software |
| **D** | Curated Turnip driver + precompiled pipeline caches | Immature Adreno 840 driver | Tracks upstream Mesa |

## Status

**Design + early implementation.** The architecture is specified and the first shippable,
tested components exist. What builds and passes tests today:

| Component | What | State |
|-----------|------|-------|
| `experiments/phase0-tso-probe/litmus` | MP + SB memory-model detector + protocol | **validated on host AND on a real S26 Ultra** — baseline confirmed WEAK ([RESULTS.md](RESULTS.md)) |
| `experiments/phase0-tso-probe/probe` | Oryon control-bit finder (kernel module) | written; builds against a target-device kernel ([Step 2 runbook](experiments/phase0-tso-probe/probe/STEP2-RUNBOOK.md)) |
| `src/kmod/oryx_memmodel` | Part A per-thread hardware-TSO driver | written; builds against a target-device kernel |
| `src/liboryxmm` | userspace TSO client for emulators | **built & tested** (fail-closed) |
| `src/liboryxcache` | Part B content-addressed translation/shader cache | **built & tested** (28 assertions) |
| `src/liboryxtu` | Deterministic x86-64→AArch64 translator + **Door 3 DRF-aware** lowering (RCpc exact-TSO, SC, and DMB-fence mappings) | **built & tested** (60 assertions) |
| `src/liboryxmap` | Offline memory-class map + **sound decision contract** + reference classifier (Door 3 no-root lever) | **built & tested** (38 assertions) |
| `src/liboryxprofile` | Part C auto-tuning profile engine | **built & tested** (27 assertions) |

Build and test everything from the repo root: `make check`. Full testing guide (host, CI, and
the on-device Phase 0 experiment): [`TESTING.md`](TESTING.md). Path from here to running a real
game with an Oryx win (honest step count): [`docs/roadmap-to-device.md`](docs/roadmap-to-device.md).
The flagship CPU-win engineering design (DRF classifier into Box64/FEX, corrected TSO mapping,
no-root validation): [`docs/box64-fex-integration.md`](docs/box64-fex-integration.md). No-root
correctness-validation plan (litmus7 battery + differential testing):
[`docs/validation-harness.md`](docs/validation-harness.md).
Launch wrapper that applies a tuned profile to a real game: [`tools/oryx-run.sh`](tools/oryx-run.sh).

The project is gated on a single decisive experiment:

### → Phase 0 is the gate: [`experiments/phase0-tso-probe/`](experiments/phase0-tso-probe/)

Part A rests on one unproven assumption: that Oryon's hardware memory-ordering mode can be
toggled per-thread from an Android kernel module (it is *documented as present* — Prism uses
it — but the toggle mechanism is not public). Phase 0 is a cheap, buildable memory-litmus
experiment that answers this before any other engineering starts. Everything downstream
branches on its result.

**Step 1 is done:** on a real S26 Ultra the cores measure **WEAK** at baseline with a
sensitive detector — the precondition confirmed on hardware ([RESULTS.md](RESULTS.md)).
**Step 2** (the root-only control-bit hunt that could prove a hardware TSO mode exists) is the
next gate — [runbook here](experiments/phase0-tso-probe/probe/STEP2-RUNBOOK.md).

## Repository layout

```
docs/
  00-architecture.md        The layer, the four parts, the evidence
  01-hardware-target.md     SM8850 / Oryon Gen 3 / Adreno 840 — the facts that constrain us
  02-memory-ordering.md     Why TSO-on-weak is the core problem
  adr/                      Architecture Decision Records (the "why nots")
  partA-hardware-tso.md     prctl + kernel-module design
  partB-translation-cache.md  Deterministic codegen + cache format
  partC-autotune.md         Profile service design
  partD-gpu.md              Driver + pipeline-cache delivery
experiments/
  phase0-tso-probe/         THE crux experiment (litmus harness + kernel probe)
```

## Provenance

Every technical claim in `docs/` is tagged with a confidence level and traces to an
adversarially fact-checked research pass over primary and peer-reviewed sources
(TOSTING/ARCS'23, Lasagne/PLDI'22, Risotto/ASPLOS'23, EuroSys'26 x86-on-RISC-V and
LLM-SBT, Google's warehouse-migration paper, Rosetta 2 reverse-engineering, Oryon
architecture deep-dives, Mesa/Turnip changelogs).

Confidence tags used throughout:
- **[Established]** — peer-reviewed or primary-source confirmed
- **[Supported]** — multiple credible sources, some inference
- **[R&D]** — plausible but unproven; requires our own experiment

> Project Oryx is a working name. This is a design proposal, not a shipped system.
