# 01 — Hardware Target

Pinning the hardware to exactly one device collapses the design space and is what makes a
shared cache possible. Oryx targets the **Samsung Galaxy S26 Ultra**, i.e. the Qualcomm
**Snapdragon 8 Elite Gen 5**.

## SoC: Snapdragon 8 Elite Gen 5

| Property | Value | Confidence |
|----------|-------|-----------|
| Part number | `SM8850-AC` (codename "Sun") | [Established] |
| CPU | 8× Oryon Gen 3 — 2 Prime @ 4.6 GHz + 6 Performance @ 3.62 GHz | [Established] |
| GPU | Adreno 840 @ 1.2 GHz, "sliced" architecture | [Established] |
| Process | TSMC N3P (3 nm) | [Established] |
| Memory | LPDDR5X, quad-channel | [Supported] |

> Note: the S26 Ultra typically ships an overclocked "for Galaxy" bin, but the underlying
> silicon and ISA are the SM8850. ISA identity across units is what Part B relies on.

## CPU: Qualcomm Oryon Gen 3 — the properties that matter for translation

| Property | Value | Why it matters | Confidence |
|----------|-------|----------------|-----------|
| **x86 memory-ordering mode** | Present in hardware; used by Windows Prism | **The headline.** Same trick as Apple's TSO bit. Dark on Android today. | [Established] hardware exists / [R&D] Android toggle |
| **x86 FP-behavior mode** | Hardware x87/x86 FP + NaN handling (FEAT_AFP-class) | Cheap, correct x87/denormal/NaN emulation | [Supported] |
| Vector width | 128-bit NEON only — **no SVE2**, no 256/512-bit | AVX/AVX2/AVX-512 must lane-split onto NEON; no wide-vector shortcut | [Supported] (confirmed gen 1; no public SVE2 in gen 3) |
| Page sizes | 4 KB & 64 KB (**not** Apple's 16 KB) | Avoids the 16 KB-page portability pain of Apple-Silicon Linux | [Supported] |
| Base ISA | ARMv8.7-class (gen 1); LSE atomics present | `casal` helps CAS translation under low contention | [Supported] |

### The two rows that define the project

The **x86 memory-ordering mode** and **x86 FP-behavior mode** are the same two hardware
accommodations Apple built for Rosetta 2. Oryon carries them because Nuvia/Qualcomm designed
it for Windows-on-ARM, and Microsoft's Prism emulator exploits them. The Android translation
stack does not touch either. Lighting up the first is **Part A**; the second is a smaller
follow-on that removes x87/FP emulation cost.

**Critical caveat.** The public record confirms:
- Oryon *has* x86 memory-ordering hardware accommodations (AnandTech / Chips-and-Cheese
  Oryon deep-dives; hwcooling). **[Established]**
- Hardware TSO as a **runtime, per-thread toggle via a system register** is documented for
  Apple (bit in `ACTLR_EL1`) and confirmed for always-on vendors (NVIDIA Carmel, Fujitsu
  A64FX). Qualcomm is **not** named in the kernel/LWN toggle-mechanism literature.
  **[Established, as a negative]**

So *whether* Oryon's mode is per-thread toggleable from an OS the way Apple's is remains
**unproven for our purposes**. This is precisely what [Phase 0](../experiments/phase0-tso-probe/)
exists to determine.

## GPU: Adreno 840 + Turnip

| Fact | Implication | Confidence |
|------|-------------|-----------|
| Adreno 840 is very new ("Gen8"/A8xx family) | Driver maturity is the bottleneck, not silicon | [Established] |
| Open **Turnip** (Mesa) driver support for A8xx arrives Mesa 26.1+/mid-2026 | Newest 8 Elite phones can currently emulate *worse* than older 8 Gen 3 | [Established] |
| Proprietary Adreno driver lacks Vulkan extensions DXVK needs | The stack depends on Turnip, not the stock driver | [Established] |
| Community Turnip forks (Kimchi, Mr. Purple, K11MCH1) add 30–50% fps | GPU driver is the single largest performance variable | [Established] |

## What the hardware rules *out*

- **SVE2-based SIMD translation** — no SVE2 on Oryon. Physically closed avenue on this chip.
- **Wide-vector AVX mapping** — no >128-bit vectors. AVX2/512 lane-split onto NEON.
- **16 KB-page-specific hacks** — not our page geometry; ignore Apple-Silicon-Linux lore here.

## Sources

Snapdragon 8 Elite Gen 5 specs: Notebookcheck, PhoneDB, NanoReview, WCCFTech, Beebom
(Qualcomm Snapdragon Summit, Sep 2025). Oryon x86 accommodations: AnandTech Snapdragon X
architecture deep-dive, Chips-and-Cheese, hwcooling.net. TSO toggle mechanism trichotomy
(Apple/NVIDIA/Fujitsu): LWN "Support for the TSO memory model on Arm CPUs" (Articles/970907);
Wrenger et al., TOSTING (ARCS'23) / *J. Systems Architecture* 2024. Turnip/A8xx status: Mesa
changelogs; community driver benchmarks.
