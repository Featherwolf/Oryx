# Getting hardware-TSO-class performance without root

The goal of Part A is to stop paying the software memory-fence tax. The cleanest way is the
hardware TSO bit — but writing that bit is a privileged operation. This document is the honest
map of what is and isn't possible **without rooting the user's phone**, and what we do about it.

## The hard boundary (why there's no user-space trick)

The Oryon memory-ordering control lives in an **EL1 (kernel) system register**. AArch64
enforces privilege in hardware: code running at **EL0 (an ordinary app) cannot read or write
EL1 registers** — the instruction faults. This is the same reason an iOS app can't flip a CPU
config bit: only the OS kernel can. So "no root" can **not** mean "our app pokes the register."
It has to mean one of the paths below.

---

## Path 1 — The kernel exposes a request syscall (true no-root, if present)

This is exactly how Apple and Windows do it **without asking the user for anything**: the OS
kernel sets the bit on the app's behalf when the app asks. On Linux/Android the mechanism is a
`prctl(PR_SET_MEM_MODEL, PR_MEM_MODEL_TSO)` — the ARM-Linux TSO interface (shipped on Asahi).
An unprivileged process can call it; if the kernel implements it and the hardware supports it,
the thread enters TSO mode. **No root.**

**We can test this right now, for free, on a stock phone.** The litmus tool has a
`--request-tso` flag that makes each thread ask the kernel, then measures:

```sh
cd Oryx/experiments/phase0-tso-probe/litmus && make
# baseline (weak) vs. kernel-granted TSO, same pair:
./oryx_litmus --test mp --iters 5000000 --cpu-a 0 --cpu-b 6
./oryx_litmus --test mp --request-tso --iters 5000000 --cpu-a 0 --cpu-b 6
./oryx_litmus --test sb --request-tso --iters 5000000 --cpu-a 0 --cpu-b 6   # control still fires?
```

Outcomes:
- **"GRANTED … MP witnesses now 0"** → 🎯 this phone's kernel already lets us do it unrooted.
  Part A works today with zero device modification. (Low probability on stock Samsung, but the
  hardware support is genuinely there — Samsung ships this silicon for Windows — so it is worth
  a definitive check, not an assumption.)
- **"request DENIED"** → the stock kernel has no memory-model prctl (the expected result).
  Then the *true* no-root hardware path is Path 2; the software path is Path 3.

## Path 2 — A custom (bootloader-unlocked, no-root) kernel carrying the bit

> **Correction (verified):** the mainline Linux `PR_SET_MEM_MODEL` prctl was **rejected** by ARM
> maintainers (Will Deacon's "strong objection" to exposing an IMPDEF opt-in). It lives only in
> the Asahi downstream tree. So "get it upstream and it ships to everyone" is **not** on the
> table — retail Android builds will not carry it.

The only hardware-TSO route that doesn't require *root* is a **custom GKI kernel flashed onto a
bootloader-unlocked device** (unlock ≠ root), carrying an Asahi-style EL1 patch that sets the
Oryon control bit. This needs only EL1, so it can live as a downstream/vendor-module patch.
Feasibility is gated on two unknowns: whether the **mobile** Oryon (S26 Ultra) actually exposes
the `ACTLR` TSO bit (it is documented only for the *laptop* Snapdragon X Oryon — the mobile bit
is **inferred, not confirmed**), and whether the device is unlockable without tripping
anti-rollback. Phase 0 Step 2 (finding the bit on a throwaway device) is the prerequisite. This
is real but narrow — **design the software path (Path 3) to degrade to a no-op if this ever
lands**, and don't depend on it for the stock-phone product.

---

## Path 3 — Don't need the bit: erase the fence tax in software (the pragmatic no-root play)

Here's the key reframing: the hardware bit is **one way** to avoid barriers, not the only way.
The barriers exist because today's translators are *conservative* — Box64's `STRONGMEM` orders
**every** memory access in case any of them races. But in a **data-race-free** program (which
well-behaved games are), ordinary accesses don't need ordering at all; only the real
synchronization does. So a smarter translator can approach hardware-TSO speed **entirely in
software, no root**, by ordering only what must be ordered:

- **DRF-aware translation.** Translate the program's *actual* atomics and locked instructions
  (`lock`-prefixed ops, `xchg`, etc.) to correct ARM acquire/release/`casal`, and leave
  ordinary loads/stores weak. For race-free code this is both correct and fast — it's the
  standard C11 mapping, not the blunt "fence everything" hammer.
- **Sharing / escape analysis.** Prove which memory is thread-local (stack, TLS, single-owner
  allocations) and skip barriers there entirely. Most memory a game touches is never
  concurrently shared; only the genuinely shared regions need ordering.
- **Optimized fence placement** (Lasagne, PLDI'22): where fences *are* needed, ~45% fewer of
  them on average, formally proven correct.
- **LSE atomics** (`casal`) for the compare-and-swaps that remain.
- **Crowd-sourced per-game memory profile** (Part C): many titles run correctly with weak
  ordering + correct atomics; the profile records which ones need extra strength, so nobody
  pays the tax who doesn't have to.

This won't always match a hardware TSO bit exactly (a truly racy program still needs
conservative ordering), but for the common case it closes most of the gap with **zero device
modification** — and it's the same codegen work that makes the [portable cache](partB-translation-cache.md)
deterministic, so it compounds.

**Full design + tested reference:** [`docs/door3-drf-translation.md`](door3-drf-translation.md).
The reference translator ([`src/liboryxtu`](../src/liboryxtu/)) implements the LOCAL/SHARED/ATOMIC
lowering with real AArch64 encodings (`LDAR`/`STLR`/`LDADDAL`/`CASAL`/`DMB`) and a test that
measures the win: on a representative block the DRF policy emits **2 ordered accesses vs the
conservative policy's 5**, running the thread-local stack traffic barrier-free.

---

## What "no root" changes about the project

Only Part A's *hardware* path depends on privilege. Everything else is already no-root:

| Part | No-root status |
|------|----------------|
| **A** — hardware TSO | Needs Path 1 (kernel prctl, test it) or Path 2 (upstream). Otherwise **Path 3 software** gets most of the win with no root. |
| **B** — translation/shader cache | ✅ pure userspace, no root |
| **C** — auto-tuning profiles | ✅ pure userspace, no root |
| **D** — driver + pipeline caches | ✅ users already swap Turnip builds without root |

## Recommended sequence

1. **Now (free, no root):** run the `--request-tso` litmus test on the S26 Ultra. Definitively
   settle whether the stock kernel grants it. (Probably no — but prove it.)
2. **Primary build:** make the DRF-aware software translator (Path 3) the default Part A story
   for unrooted users. It's real engineering, not a hardware gamble, and it benefits everyone.
3. **Long game:** pursue the upstream `PR_SET_MEM_MODEL` (Path 2) for the true no-root hardware
   mode — using whatever Phase 0 Step 2 finds on a throwaway device as the patch's basis.

The rooted-device Step 2 experiment still matters — it's how we *discover the bit* and prove
the hardware mode is real. But shipping it to real users routes through Path 2/3, not through
asking millions of people to root their phones.
