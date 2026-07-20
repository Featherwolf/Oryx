# Door 3 — DRF-aware software translation (no root)

The no-root way to recover most of the hardware-TSO win: **order only what must be ordered.**

Reference implementation (tested): the memory-model lowering in
[`src/liboryxtu`](../src/liboryxtu/) (`oryx_translate_ex` + `oryx_mm_stats`).

## The problem it solves

To be correct, an x86→ARM translator must recover the ordering ARM's weak model drops. The
blunt way (Box64 `STRONGMEM`, and FEX's default) is to order **every** memory access — turn
every load into an acquire and every store into a release. That is the "software fence tax":
it is correct, but it pays ordering cost on accesses that can never be observed by another
thread. The DRF-aware translator pays that cost **only where a data race could actually
happen**, and runs everything else at full weak-memory speed.

## The core idea (data-race-free reasoning)

In a properly synchronized program, inter-thread ordering is only observable **at
synchronization** (atomics, fences). For race-free ordinary accesses, weak ordering is
indistinguishable from TSO. So:

1. Translate the program's **actual synchronization** precisely (this is non-negotiable for
   correctness).
2. Give **potentially-shared** ordinary accesses TSO-equivalent ordering (acquire/release).
3. Run **provably-unshared** ordinary accesses as plain weak loads/stores — free.

## The per-operation decision

| Guest x86 operation | Class | AArch64 lowering | Ordered? |
|---------------------|-------|------------------|----------|
| Ordinary load, **thread-local form** (`[RSP/RBP+disp]`, `FS:`/`GS:` TLS) | LOCAL | `LDR` (offset) | **no** |
| Ordinary store, thread-local form | LOCAL | `STR` (offset) | **no** |
| Ordinary load, **general pointer** (maybe shared) | SHARED | `LDR; DMB ISHLD` (exact-TSO) or `LDAR` (RCsc, conservative) | yes |
| Ordinary store, general pointer | SHARED | `DMB ISHST; STR` (exact-TSO) or `STLR` | yes |
| `LOCK`-prefixed RMW, `XCHG` mem (implicitly locked) | ATOMIC | LSE acq+rel — `LDADDAL` / `SWPAL` / `CASAL` … | yes |
| `MFENCE` / `LFENCE` / `SFENCE` | FENCE | `DMB ISH` / `ISHLD` / `ISHST` | yes |

Ordered SHARED accesses require the effective address in a register first (the acquire/release
register forms take no offset), so `[base+disp]` becomes `ADD xTMP, base, #disp` then
`LDAR/STLR [xTMP]` — an extra reason LOCAL (single `LDR` with a built-in offset) is cheaper.

## Why it's sound — even for racy binaries

- **Atomics & fences** map to correct ordered ARM sync → all real synchronization is preserved.
- **SHARED** ordinary accesses use acquire/release → TSO-equivalent ordering for everything
  another thread can observe.
- **LOCAL** accesses are plain/weak, but a **release store orders all preceding stores**
  (weak ones included) before it — so the publication pattern *(write private data → publish a
  pointer with a release store; other thread acquires the pointer → reads the data)* is
  correct even though the data writes were weak.
- **Classification is by addressing *form*, not by object.** An escaped stack object is read by
  another thread through a *general pointer* → SHARED form → ordered on the reader; the writer's
  release-store handles the publish. So escape is handled automatically.

The **only** unsound case: a genuine data race on memory accessed through *thread-local
addressing form* on the writing side — i.e. `RSP`/`RBP` actually aliases memory shared with
another thread (shared stacks, some fiber/coroutine libraries). This is rare, and covered by a
per-game **`strong-stack`** override (below). Default-on; opt to conservative when a title
needs it.

## The classifier (static, per memory operand)

- base register is `RSP` or `RBP`, displacement within the frame → **LOCAL (stack)**
- `FS:`/`GS:` segment override (x86-64 TLS base) → **LOCAL (TLS)**
- `RIP`-relative into read-only data → **LOCAL** (no ordering needed for immutable data)
- anything else (general-register base — could be shared heap) → **SHARED** (conservative default)

Refinements layered on top when available: allocator-tagged single-owner regions; escape
analysis for freshly-`malloc`'d, not-yet-published memory; and crowd-sourced per-game
overrides (Part C).

## Strength selection (use the cheapest *correct* primitive)

> **Correction (verified against the ARM memory model).** An earlier draft claimed `LDAPR`
> (RCpc) + `STLR` equals x86-TSO. **It does not — RCpc is strictly *weaker* than TSO.** RCpc
> relaxes multi-copy atomicity, so `LDAPR` can read a store-release value not yet globally
> observed, permitting **WRC-style causality violations that x86-TSO forbids** (x86-TSO is
> multi-copy atomic). So bare `LDAPR` is **unsound** for SHARED accesses.

The two correct choices for a SHARED access are:

- **Minimal exact-TSO — the DMB-fence scheme** (Arancini-proven, multi-copy-atomic on ARMv8):
  load → `LDR; DMB ISHLD`, store → `DMB ISHST; STR`, `MFENCE` → `DMB ISH`. Each fence is proven
  necessary-and-sufficient, and adjacent fences can be merged/eliminated. This preserves exactly
  the W→R relaxation TSO allows.
- **Conservative — acquire/release** `LDAR`(RCsc)/`STLR`: *stronger* than TSO (it also forbids
  the W→R reorder, i.e. it's sequentially consistent for those accesses) — correct, simpler, but
  over-costs. **This is what the reference `liboryxtu` emits** — safe by construction, and the
  right baseline for differential correctness testing.

`LDAPR` is only usable in a mapping that *separately* restores multi-copy atomicity; it is not a
drop-in "cheaper LDAR." See [`docs/box64-fex-integration.md`](box64-fex-integration.md) §4 for
the full mapping table and the special cases (`MOVNT*`, mixed-size, `REP` string ops).

## Expected win

Real code is dominated by stack/TLS traffic — locals, register spills, call frames, arguments.
Classifying those LOCAL removes ordering from the **majority** of memory operations; only
genuinely-shared heap pointers and real atomics keep ordering. That closes most of the gap to
a hardware TSO bit, in pure software, with **zero device modification**.

The reference translator makes this measurable: `oryx_mm_stats` reports plain vs. ordered
counts, and the test suite asserts the DRF policy emits **far fewer ordered accesses** than the
conservative policy on a representative block.

## Integration with the rest of Oryx

- **Part C (profiles):** each profile carries a `memmodel` policy — `drf` (default), or
  `strong-stack` / `conservative` for the rare racy title, or `hw_tso` where Part A is
  available. Crowd telemetry (crash rate) surfaces titles that need a stronger setting.
- **Part B (cache):** the policy is folded into `profile_id`, so DRF-translated and
  conservatively-translated blocks are distinct cache entries and never mixed (already handled
  by the format).
- **Part A (hardware TSO):** the *same* translator, one policy switch — `hw_tso` emits all
  ordinary accesses plain and lets the silicon order them. Door 3 and Part A are two policies of
  one codegen path, not two codebases.

## Honest limitations

- This is not a machine-checked proof; the stack-aliasing corner is a real (rare) soundness gap,
  mitigated by the `strong-stack` override, not eliminated.
- Non-temporal stores (`MOVNT*`), string operations (`REP MOVS/STOS`), and self-modifying code
  need their own handling and are out of scope for the ordering classifier.
- The real work is applying this in Box64/FEX codegen; `liboryxtu` demonstrates the lowering and
  the metric, and defines the contract that codegen must meet.
