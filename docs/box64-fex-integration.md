# Integrating DRF-aware memory-model translation into Box64/FEX (no root)

This is the concrete engineering design for the flagship CPU win — grounded in an
adversarially-verified research pass over Box64/FEX internals, the x86→ARM memory-model
literature (Lasagne, Risotto, Arancini, EuroSys'26 Box64-on-RISC-V), and the no-root
constraint. Confidence tags: **[V]** verified against a primary source, **[V!]** corrected a
wrong assumption during verification, **[R&D]** plausible/unproven.

## 0. The realistic target (set expectations honestly)

Rosetta 2's ~0% ordering overhead comes **entirely** from Apple's hardware TSO bit — a
kernel-privileged config register the scheduler toggles per timeslice. There is *no software
fence fallback* in the Rosetta path. **[V]** That bit is EL1-only and unreachable without root,
and the upstream Linux `PR_SET_MEM_MODEL` prctl to expose it was **rejected by ARM maintainers**
(Will Deacon's "strong objection"); it lives only in the Asahi downstream tree. **[V!]** So the
no-root ceiling is **software**, and the right benchmark is the EuroSys'26 Box64-on-RISC-V
result: **< 40% overhead vs native** with a pure-software TSO method on genuinely weak (RVWMO)
hardware — 3.3× faster than QEMU-TCG. **[V]** Treat **< 40%**, not ~0%, as the target.

## 1. Base: prototype in FEX, ship in Box64

| | FEX | Box64 |
|---|---|---|
| Structure | **SSA IR** (`OpcodeDispatcher`) with per-access ordering ops: `LoadMem`/`LoadMemTSO`/`ParanoidLoadMemTSO`, ditto stores **[V]** | Per-x86-opcode **direct emitter**, no IR; ordering is a **global** knob (`STRONGMEM` 0–4) **[V]** |
| Adding a classifier | An **IR pass** that *demotes* proven-DRF `*TSO` ops to plain ops — **no backend change** (plain-op lowering to bare `LDR/STR` already exists) **[V]** | **Emit-level surgery**: every opcode handler must consult a region/class side-table |
| Precedent | Already consumes MSVC `/volatileMetadata` — a per-access ordering hint channel = a DRF-classifier precursor **[V]** | `wine` "volatile ranges" disable STRONGMEM wholesale (coarse) **[V]** |
| Android role | Compatibility profiles (e.g. GameNative Skyrim LE) **[V]** | **Default workhorse** (Winlator/GameNative/GameHub) **[V]** |

**Decision:** build and *prove correct* the DRF classifier as a **FEX IR pass** (SSA def-use
chains make escape analysis tractable; op-demotion is the whole output surface), then **transplant
the decided policy into Box64** for shipping performance. Validate in the clean substrate; deploy
in the fast one.

## 2. Box64 hook points (for the shipping port) [V]

Everything is userspace codegen — **no kernel privilege**.

| What | Where |
|------|-------|
| Loads | `GETED*/GETEB*` macros → call `SMREAD()` then `LDxw` — `src/dynarec/arm64/dynarec_arm64_helper.h` |
| Stores | `WBACK/EWBACK/EBBACK` → `STxw` then `SMWRITE()` — same file |
| Ordering choke points (arch-neutral) | `SMREAD/SMWRITE/SMWRITE2/SMEND/WILLWRITE` + `*LOCK` variants — `src/dynarec/dynarec_helper.h` |
| Address class (the signal) | `geted()` (ModRM base = `TO_NAT((nextop&7)+(rex.b<<3))`; RSP=4, RBP=5) and `grab_segdata()` (FS/GS=TLS) — `dynarec_arm64_helper.c` |
| Per-insn metadata to extend | `will_write:2; will_read:1; last_write:1; lock:1` in `dynarec_arm64_private.h` |

**Plan:** add `mem_class:2` next to those bitfields; populate it in **STEP 1** (analysis) from
`geted()`'s base/segment info; consume it in **STEP 3** where `DMB_*` is emitted, by passing the
class into `SMREAD/SMWRITE` (which today take no address argument). This mirrors the existing
two-pass STRONGMEM flow exactly, at zero extra runtime cost. Today STRONGMEM is **per-SEQ, not
per-address**, and **never consults address class** — that gap is the entire opportunity. **[V]**

## 3. The classifier — layered, over-approximating SHARED

**The cardinal rule:** misclassifying **SHARED as LOCAL is a correctness bug** (lost ordering);
**LOCAL as SHARED only costs performance**. So every layer must **over-approximate the SHARED
set** — absence of evidence of privacy ⇒ SHARED. **[V]**

- **Layer 0 — minimal verified fences (unconditional).** Emit the proven-minimal mapping (§4) so
  every fence you *do* keep is necessary-and-sufficient.
- **Layer 1 — static addressing form (Lasagne-style).** Use-def walk of the pointer to a stack
  slot or FS/GS(TLS) base ⇒ LOCAL; everything else SHARED. This single heuristic is what buys
  Lasagne's **45.5% avg / 65% max** fence reduction. **[V]** Invest in IR-refinement
  (pointer-param promotion, int→GEP recovery) or the walk fails and everything degrades to SHARED.
  - **Escape downgrade (required for soundness):** a stack/TLS address that flows to a global
    store, a cross-thread argument, or the `clone`/`pthread_create` boundary → mark its region
    SHARED from that point. Intercept `clone` as the primary tripwire. **[V]**
  - **Fiber/coroutine guard:** disable stack=LOCAL when the guest manipulates SP outside call/ret
    (`makecontext`/`swapcontext`, Go/.NET/JVM guests) — a switched stack isn't single-owner. **[V]**
  - **TLS caveat:** FS/GS *slot* is LOCAL, but a pointer *loaded from* TLS often points to shared
    heap — classify transitive loads by their own provenance. **[V]**
- **Layer 2 — dynamic ownership (no root).** Allocator interposition tags each `malloc`/`mmap`
  region single-owner at allocation; **ARMv8.5 MTE** (EL0-reachable on Android 12+ via
  `prctl(PR_SET_TAGGED_ADDR_CTRL)`) acts as a "first cross-thread access" oracle — tag owned
  regions, run SYNC mode, treat `SEGV_MTESERR` as the promote-to-SHARED signal. HMTRace shows
  **~4% time overhead** is achievable. **[V]** Promotion is one-way and triggers **re-translation**
  of the affected block to the fenced variant (never a silent violation).
- **Layer 3 — offline AOT metadata (the load-bearing product lever).** Run whole-binary
  alias/escape/MHP analysis **off-device** (optionally LLM-assisted), ship per-basic-block
  `LOCAL/SHARED/UNKNOWN` bits via **liboryxcache** keyed to `build-id + block addr`. Default
  UNKNOWN → SHARED. An offline LOCAL is a **hint gated by a cheap on-device guard** (allocator/MTE
  tag, or the on-device static walk also proving it), **never authoritative** — ASLR, JITed guest
  code, and input-dependent races mean offline soundness isn't transferable. Bind to a strong
  binary hash; stale/mismatched metadata degrades to conservative fences. **[V]** Precedent:
  Rosetta's persistent `.aot` cache, MSVC `/volatileMetadata` (FEX gets "dramatic speedup" from
  it). **[V]**

## 4. The correct x86-TSO → AArch64 mapping [V!]

x86-TSO forbids all reorderings **except store→load**, and is **multi-copy atomic**. The mapping
must preserve R→R, R→W, W→W, may relax W→R, **and preserve multi-copy atomicity**. Crucially,
ARMv8 is **other-multi-copy-atomic** — so an acquire/release mapping already preserves the
multi-copy-atomicity TSO needs; WRC and IRIW stay forbidden. The three sound mappings:

| x86 | ✅ Exact-TSO, cheapest (needs 8.3) | Exact-TSO, portable | Correct-but-stronger |
|-----|-----------------------------------|---------------------|----------------------|
| load | **`LDAPR` (RCpc)** | `LDR; DMB ISHLD` (trailing read fence) | `LDAR` (RCsc → SC) |
| store | `STLR` | `DMB ISHST; STR` (leading write fence) | `STLR` |
| MFENCE | `DMB ISH` | `DMB ISH` | `DMB ISH` |
| LOCK RMW / XCHG / CMPXCHG | LSE **`-AL`** atomic (`CASAL`/`SWPAL`/`LDADDAL`) — full acq+rel | same | ll/sc `LDAXR/STLXR` loop |

**`LDAPR`/`STLR` is the exact minimal TSO mapping — and the primary choice on the target.** [V!]
RCpc differs from RCsc (`LDAR`) by exactly one edge: it drops the `STLR`→`LDAPR` (W→R) ordering.
That W→R relaxation — the store-buffer / `SB` outcome — is *precisely* what x86-TSO already permits,
so `LDAPR`/`STLR` = SC **minus** the one edge TSO also drops = **exactly x86-TSO**. It costs zero
extra instructions and is what **FEX emits in production** for TSO loads. (The "RCpc is too weak"
folklore is about C++ `seq_cst`, whose distinguishing failure *is* `SB` — a test TSO allows; it
does not apply to TSO emulation, where `SB` is permitted and WRC/IRIW remain forbidden.) `LDAPR`
needs `FEAT_LRCPC` (ARMv8.3) — Oryon has it. For pre-8.3 cores, the **DMB-fence scheme** is the
portable exact-TSO fallback and also enables cross-access fence elimination/merging
(Risotto/Arancini). `LDAR` (RCsc) is *stronger* than TSO (forbids the W→R reorder TSO allows) —
correct but over-costs; it is the right **conservative baseline** for differential testing and is
what the reference `liboryxtu` emits by default (safe). Caveat: `LDAR`/`STLR` require natural
alignment, so a maybe-unaligned SHARED access uses `LDAPR`/DMB, never SC.

**Special cases that break naive mappings** (must be handled explicitly): non-temporal stores
`MOVNT*`/WC memory (lower to plain `STR` + explicit `DMB` for `SFENCE`; never fold into the TSO
path); mixed-size / misaligned accesses and `REP MOVS/STOS` (intra-instruction `si`-relation
ordering — decompose + fence, per Arancini). **[V]**

## 5. No-root correctness validation (serves the whole constraint) [V]

You cannot trust a weak translation without proving it doesn't lose ordering — and you have no
hardware-TSO reference on the device. Three complementary layers, **all no-root**:

1. **On-device litmus battery — userspace `litmus7`** (herdtools) runs on Android with **no root,
   no kernel module** (klitmus7 needs a module → out). Emit the mapped code for **MP, SB, LB, R,
   S, 2+2W, WRC, ISA2, IRIW** and assert: TSO-**forbidden** outcomes **never** appear, **and** the
   one TSO-**allowed** outcome (SB store→load) **is** observed (else you're accidentally
   over-strong, masking a bug). **WRC/IRIW specifically probe the multi-copy-atomicity property**;
   the `LDAPR`/`STLR` mapping must keep them forbidden (ARMv8 other-MCA guarantees this) — run them
   to confirm the mapping did not accidentally reach for a non-MCA primitive. Our Phase-0 MP/SB
   harness is the 2-thread seed; the 3–4-thread cases are best generated by `litmus7`.
2. **Differential testing on device:** run each workload under the **weak** (Door-3) translation
   and the **conservative** (`LDAR`/full-DMB) translation on identical inputs/seeds; any divergence
   flags a missing-fence bug. Pair with a tsan-style race detector + record/replay for repro.
3. **Offline, device-independent:** encode the mapped sequences as a **herd7 `.cat`** check;
   highest assurance is adopting/porting a **Lasagne/Arancini machine-checked** mapping so
   correctness never rests on one device's observed behavior.

## 6. No-root avenues, ranked — and dead-ends killed [V]

**Pursue:**
1. **Offline sharing-analysis → AOT memory-class cache (liboryxcache).** The core product lever
   (Layer 3 above). Sound by conservative over-approximation.
2. **MTE dynamic confirmer** (Android 12+, EL0 via prctl). Optional per-device upgrade to Layer 2.
3. **Custom GKI kernel via bootloader-unlock-without-root** — the only middle path to the *hardware*
   TSO bit, since the Asahi-style enablement needs only EL1. Gated on Qualcomm exposing the mobile
   Oryon `ACTLR` bit (**undocumented — the S26-Ultra bit is inferred, not confirmed** **[V!]**) and
   the device being unlockable. Design the software path to **degrade to a no-op** if this ever
   lands.

**Killed (do not revisit):** FEAT_TME/HTM rollback (withdrawn by ARM; Oryon is v8.7 → absent);
software speculate-and-squash (checkpoint cost > fence cost in hot loops); page-permission fencing
(`mprotect` trap + false sharing — MTE supersedes it); NPU/coprocessor offload (ordering is a
store-buffer/pipeline property, not offloadable); userspace hypervisor (no EL2/KVM on stock, and a
guest EL1 still can't set IMPDEF ACTLR); mainline upstream adoption (rejected); **crash-telemetry
as a relaxation *driver*** (heisenbugs rarely reproduce — telemetry may only **tighten** to TSO as a
kill-switch, never license a new relaxation). **[V]**

## 7. One fork, multiple wins — the adjacent CPU taxes [V]

Flags and memory ordering are the two dominant CPU taxes and are decided at the **same per-opcode
emit site**, so one fork + one forward analysis pass delivers both:

- **Cross-block flag-liveness (win #2).** SF/ZF/CF/OF map ~1:1 to ARM NZCV, but **PF (parity) and
  AF (adjust) have no ARM equivalent** and cost multi-instruction software sequences. (Rosetta gets
  them free via an Apple NZCV-bits-26/27 extension — **not available on Oryon**.) The win is
  eliding **dead** PF/AF computation across the block-linking graph. Box64 already has
  `NATIVEFLAGS` (default on) and `SAFEFLAGS` (default on, 0–2); deferred flags are an always-on
  internal mechanism (**there is no `BOX64_DYNAREC_DEFERRED` env var** **[V!]**). The fork's value
  is stronger *cross-block* liveness.
- **FEAT_AFP (win #3, free, userspace).** ARMv8.7 (**Oryon has it**) `FPCR.AH/FIZ/NEP` make ARM
  denormal/NaN corner cases match x86; set once per thread, drop the software SSE normalization
  fix-ups. No root, no kernel. Pair with RPRES for reciprocal/rsqrt.
- **x87 80-bit (win #4):** Box64's hardware-double-with-escalation beats FEX/QEMU softfloat; gate
  by the same classifier.
- **Block-linking/IR-cache (#5):** connective tissue — the same cross-block graph flag-liveness and
  DRF analysis both need; improving it makes wins #1–2 span blocks.

**ROI order for one fork:** (1) DRF memory classifier → unordered loads/stores where safe;
(2) cross-block flag-liveness → elide dead CF/OF/PF/AF; (3) FEAT_AFP FPCR setup; (4) x87
hardware-double escalation; (5) block-linking. **Items 1, 2, 5 are literally the same
forward-analysis + emit-path change** — which is the whole reason one fork delivers all of them.

## 8. Phased plan

| Phase | Deliverable | No root? |
|-------|-------------|----------|
| **1** | FEX IR pass: demote proven-DRF `*TSO` ops (Layer 1 static + escape downgrade), behind a config flag | ✅ |
| **2** | No-root validation harness: `litmus7` battery (incl. WRC/IRIW) + differential testing vs conservative | ✅ |
| **3** | Offline analysis → liboryxcache memory-class metadata (Layer 3); on-device guard | ✅ |
| **4** | Transplant the validated policy into Box64 (`mem_class` field + SM-macro hooks) for shipping perf | ✅ |
| **5** | Adjacent wins in the same fork: cross-block flag-liveness, FEAT_AFP | ✅ |
| **6 (opt)** | MTE dynamic confirmer; custom-GKI hardware-TSO backend that no-ops the software path | MTE ✅ / GKI = unlock, no root |

Everything that ships to a stock phone is **Phases 1–5, all no-root**. Phase 6 is optional upside.
