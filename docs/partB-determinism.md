# Part B — Deterministic codegen: the contract and how Box64 meets it

The portable cache ([ADR-0002](adr/0002-content-addressed-portable-cache.md)) only works if a
translation of the same guest block produces **byte-identical** host code on every device and
every run. Today's dynarecs are not deterministic. This document defines the contract, lists
the specific sources of non-determinism in a Box64-class translator and the fix for each, and
describes the conformance gate that keeps non-deterministic output out of the shared cache.

A working, tested demonstration of the contract lives in
[`src/liboryxtu`](../src/liboryxtu/): a reference x86-64→AArch64 basic-block translator that is
byte-deterministic by construction and feeds straight into `liboryxcache`. It exists to prove
the pipeline and pin the contract — it is not Box64.

## The determinism contract

A translation unit is cacheable iff, for a fixed
`(guest_bytes, translator_version, profile_id, isa_id)`:

1. **Reproducible** — two independent translations yield identical serialized bytes (hence an
   identical content address).
2. **Position-independent** — no host addresses appear in `code[]`; every guest→host target is
   a relocation resolved at map time.
3. **Self-contained** — the unit depends only on its inputs, not on process/global state
   (heap layout, ASLR, thread timing, environment).

`liboryxtu` satisfies all three; the tests assert (1) directly (two translations → identical
content address) and (2) structurally (branch targets are `ORYX_RELOC_BRANCH_GUEST_PC`
relocations, never baked addresses).

## Non-determinism in a Box64-class dynarec, and the fix

| # | Source | Why it breaks determinism | Fix |
|---|--------|---------------------------|-----|
| 1 | **Embedded host addresses** — direct branch/call targets patched to absolute host PCs; jump-table pointers | Host code buffer is mmap'd at an ASLR'd address, different every run | Emit PC-relative branches with **relocations** to guest PCs (as `liboryxtu` does); resolve at map time |
| 2 | **Block placement / linking order** — the order blocks are JIT'd depends on execution path and timing | Same block gets different neighbors / link state | Cache at **basic-block granularity**, each block self-contained; linking is a runtime step over relocations, not baked into bytes |
| 3 | **Register allocator nondeterminism** — allocation influenced by hashed pointers or discovery order | Different host register choices for identical input | Deterministic allocation: fixed guest→host mapping (or a pure function of the IR only). `liboryxtu` uses a fixed map |
| 4 | **Constant pool / literal ordering** — literals interned via pointer-keyed hash maps | Iteration order varies by allocation address | Order the pool by first-use index (a pure function of the instruction stream) |
| 5 | **Hash-map iteration order** anywhere in codegen | Address-seeded ordering | Replace with insertion-ordered or sorted structures for anything that reaches emitted bytes |
| 6 | **Timestamps / build ids / counters** embedded in output | Vary per run | Exclude from `code[]`; put translator identity in `translator_version` only |
| 7 | **Uninitialized padding** in emitted structures | Random stack/heap bytes leak into output | Zero-init all serialized structures (the `OTU1` writer builds a fully-defined byte stream) |
| 8 | **Feature/tuning flags** read from environment at translate time | Same binary, different flags → different code, same key | Fold every codegen-affecting flag into `profile_id`, which is part of the key (distinct flags ⇒ distinct entries) |
| 9 | **Self-modifying / runtime-generated code** | Cannot be predicted ahead of time | Out of scope for caching — the on-device JIT keeps handling it; only stable blocks are cached |

Items 1–8 are engineering, not research. Item 9 is why the JIT never goes away (see
[Part B design](partB-translation-cache.md)).

## The conformance gate

Determinism is enforced, not assumed:

1. **Translate-twice check (build/CI).** For a corpus of blocks, translate each twice in one
   process and once in a **separate** process; require identical content addresses. `liboryxtu`
   passes the in-process form in its test suite; the cross-process form is a CI wrapper that
   runs the translator binary twice and diffs.
2. **Producer agreement (network).** The CDN accepts a warmed unit for a `(logical_key)` only
   after **N independent devices** submit byte-identical content for it. A device whose output
   disagrees is quarantined (its translator build is flagged non-conformant) rather than
   poisoning the cache.
3. **Consumer verification (always).** Every fetched unit is re-hashed against its content
   address before any page is made executable (`liboryxcache` fail-closed integrity). Even a
   determinism bug that slipped both gates cannot execute mismatched bytes.

## Applying this to Box64

Box64's dynarec already emits per-basic-block code and supports a code cache, so the structure
is amenable. The concrete work items, in order:

1. Route all direct control-flow through a relocation table keyed by guest PC (item 1) — the
   largest change, and the one `liboryxtu` demonstrates end to end.
2. Make register allocation and any constant/literal handling a pure function of the decoded
   block (items 3–4).
3. Audit codegen for map iteration, timestamps, and padding (items 5–7).
4. Hash all codegen-affecting `BOX64_*` flags into `profile_id` (item 8).
5. Add the translate-twice conformance target to Box64's test matrix (gate 1).

Until a block's translator build is conformant, its output is simply not published to the
shared cache — the device still runs it locally via the normal JIT. Determinism is thus an
**opt-in, per-translator-version** property, adopted incrementally without risking correctness.
