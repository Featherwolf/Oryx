# 00 — Architecture

## What Oryx is

A middleware layer beneath the existing Android x86 gaming stack. It does **not** replace
Box64, FEX, Wine/Proton, DXVK/VKD3D, or Turnip. It changes how they are *fed* and *tuned*,
and — where the hardware and privilege allow — how they handle memory ordering.

```
        ┌──────────────────────────────────────────────┐
        │   GameNative / GameHub / Winlator (front-end) │
        └──────────────────────────────────────────────┘
                              │  (existing hooks: backend select,
                              │   driver swap, per-game config)
        ┌─────────────────────▼────────────────────────┐
        │                 O R Y X                        │
        │  ┌────────────┐  ┌───────────┐  ┌───────────┐ │
        │  │ A. HW-TSO  │  │ B. Cache  │  │ C. Tuner  │ │
        │  │  capability│  │  client   │  │  profiles │ │
        │  └─────┬──────┘  └─────┬─────┘  └─────┬─────┘ │
        │        │               │              │        │
        │  ┌─────▼───────────────▼──────────────▼─────┐ │
        │  │  D. Curated Turnip + pipeline caches      │ │
        │  └───────────────────────────────────────────┘ │
        └─────────────────────┬──────────────────────────┘
                              │
   ┌──────────────┬───────────┴───────────┬───────────────┐
   │  Box64 / FEX │  Wine / Proton        │  DXVK / VKD3D │
   │ (x86→ARM)    │  (Win32/64 API)       │  (D3D→Vulkan) │
   └──────┬───────┴───────────────────────┴───────┬───────┘
          │                                        │
   ┌──────▼─────────┐                   ┌──────────▼────────┐
   │ Oryon Gen 3 CPU│                   │  Adreno 840 (Turnip)│
   │  hardware TSO ✦│                   │                     │
   └────────────────┘                   └─────────────────────┘
     ✦ = the dormant capability Part A unlocks
```

## The two independent fronts

Benchmarks are unambiguous: translation overhead only bites **CPU-bound** code. On
GPU-bound work every translator is already near-native (glmark2: Box64 178 fps vs native
181 fps). **[Established]** So we fight on two separate fronts and must win both:

1. **CPU front** — the x86→ARM translation tax, dominated by software memory-ordering
   emulation. Owned by Parts **A** (eliminate it in hardware) and **B** (never redo the
   translation work).
2. **GPU front** — driver maturity and shader-compilation stutter, *not* API-translation
   throughput. Owned by Parts **D** (driver) and **B** (precompiled shader delivery).

Part **C** (auto-tuning) spans both by picking the winning configuration per game.

## Design principles

1. **Hardware-first.** The single biggest available win on *this specific chip* is a
   hardware feature that is already present and merely unused. Chase that before chasing
   software cleverness. See [ADR-0001](adr/0001-hardware-first-not-new-emulator.md).
2. **Fixed target = shareable work.** Every S26 Ultra is ISA-identical. Any translation or
   shader produced on one device is byte-valid on all of them. This is what makes a
   *portable* cache possible where per-machine caches (Rosetta's `/var/db/oah`) stop.
   See [ADR-0002](adr/0002-content-addressed-portable-cache.md).
3. **Extend, don't fork.** GameNative already exposes every seam Oryx needs (Box64 *and*
   FEX backends, swappable Turnip drivers, per-game config). Oryx populates those seams.
   See [ADR-0003](adr/0003-extend-gamenative-not-fork.md).
4. **Degrade honestly.** Part A may prove impossible on stock hardware. Every part must
   deliver value without it, and the fallbacks are named, not hand-waved.

## Sequencing (de-risk the hard bet first)

| Phase | Deliverable | Depends on |
|-------|-------------|------------|
| **0** | Prove Oryon's TSO bit is toggleable from a kernel module | — (the gate) |
| **1** | Deterministic codegen + translation/shader CDN (B, D) | independent of Phase 0 |
| **2** | Wire hardware ordering into Box64/FEX (A) | Phase 0 = pass |
| **3** | Upstream into GameNative/GameHub; push kernel support upstream | 1, 2 |

Part **C** ships in parallel from day one — it is pure software and useful immediately.

## Why the gap exists at all

The Rosetta gap is not a compiler-quality gap. On identical M1 hardware Rosetta 2 reaches
~71% of native on 7-zip while Box64 reaches ~57% — and the difference is that Rosetta uses
the hardware TSO + FP modes and Box64 emulates ordering in software. **[Established]** The
whole of Oryx is a plan to give the Android stack that same hardware advantage on the one
Android chip that has the hardware for it.
