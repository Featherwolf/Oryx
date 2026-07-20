# ADR-0001 — Hardware-first, not a new emulator

- **Status:** Accepted
- **Date:** design phase

## Context

The Android x86-gaming stack (Box64/FEX + Wine + DXVK → Turnip) is mature and community-
maintained. The temptation is to write a "better emulator." But the measured ceiling for
on-device JIT dynarec is ~57% of native (Box64 on 7-zip), while Rosetta reaches ~71% on the
*same* hardware — and the delta is hardware TSO + hardware x86 FP, not translation cleverness.
The literature (Lasagne, Risotto) shows software-only ordering optimizations move the needle
by single-to-low-double digits at most.

## Decision

Do **not** build a new emulator. Build a layer that (a) unlocks the hardware feature that
separates the ~57% and ~71% tiers, and (b) removes redundant work (re-translation,
re-compilation) around the existing emulators.

## Consequences

- **Positive:** We inherit years of Box64/FEX/Wine/DXVK/Turnip compatibility work instead of
  re-litigating it. Our effort concentrates on the one thing none of them can do from
  userspace — reach the privileged memory-mode control.
- **Positive:** Every part degrades to "still useful" if the hardware bet fails.
- **Negative:** We take a hard dependency on reverse-engineering an undocumented Qualcomm
  feature (mitigated by making it Phase 0, the cheap gate).
- **Negative:** We must track upstream churn in five external projects.

## Alternatives rejected

- **Pure LLM / static AOT translation** (EuroSys'26 LLM-SBT reaches near-native): cannot
  handle self-modifying code, runtime JITs, anti-cheat, or DRM packers — i.e. exactly what
  games ship. Kept only as an *offline* booster for static hot paths inside Part B.
- **Recompile from source** (Google's warehouse path): impossible for closed commercial
  games; we only ever have binaries.
