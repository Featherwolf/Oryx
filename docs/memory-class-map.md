# Format — offline memory-class map (`OMC1`)

Reference implementation: [`src/liboryxmap`](../src/liboryxmap/) (tested). Design context:
[`docs/box64-fex-integration.md`](box64-fex-integration.md) §3 (Layer 3), Door 3.

## What it is and why

Deciding whether an access is thread-**LOCAL** (safe to run weak) or **SHARED** (must be
ordered) is the precision driver for the no-root memory-model win — and doing it *well* needs
heavy escape/alias analysis that is too expensive to run on-device. So it runs **off-device** on
a guest module and ships a **memory-class map**: per-access `{UNKNOWN, LOCAL, SHARED, ATOMIC}`
annotations the on-device translator consults to pick each access's ordering. The map travels
through [`liboryxcache`](../src/liboryxcache/), keyed to the module's hash.

## The soundness contract (the whole point)

Misclassifying **SHARED as LOCAL is silent memory corruption**; **LOCAL as SHARED only costs
speed**. So the map and its consumer are built to **never relax on doubt**:

| Map says | Runtime guard | Ordering emitted |
|----------|---------------|------------------|
| `ATOMIC` | — | atomic (LSE) |
| `LOCAL`, no guard required (statically proven) | — | **relax** (plain weak) |
| `LOCAL`, guard required | confirmed | **relax** |
| `LOCAL`, guard required | not confirmed | **ordered** (fail-safe) |
| `LOCAL`, **ESCAPED** (escape analysis found it can be shared) | — | **ordered** (downgraded to shared) |
| `SHARED` | — | ordered |
| `UNKNOWN` / no entry / unparsable / identity mismatch | — | ordered |

The `ESCAPED` flag overrides the `LOCAL` class byte unconditionally: escape analysis is the writer
side of the contract, so if the off-device pass ever finds a `LOCAL`-form access can leak, it
stamps `ESCAPED` and the access orders regardless of any guard. "Escape-proof automatic" (the
guard-free relax row) is reserved for accesses whose address is *provably never taken* — register
spills, statically-resolved frame slots — never a module-scope "only this module touches it"
argument, which is not robust to `dlopen`/JIT/self-modifying guest code.

This is exactly `oryx_mmap_decide()` — the single function where the contract lives. An offline
`LOCAL` is only ever a **hint**: it relaxes ordering only when the analysis needed no runtime
confirmation, or a runtime ownership/MTE guard confirms it. Everything else orders. The default
for anything the map doesn't cover is **order**.

## Producer obligation

The off-device analyzer must be a **conservative over-approximation**: default every access to
SHARED, downgrade to LOCAL *only on proof* (no address-taken escape, no global/param aliasing,
single-threaded region). Any access an escape/alias pass can't clear stays SHARED. Absence of
evidence of privacy is **not** evidence of privacy.

The bundled `oryx_mmap_classify()` is a reference producer over the `liboryxtu` guest IR: only
`RSP`/`RBP`-relative (stack) accesses become `LOCAL` — and always with `GUARD_REQUIRED`, because
a stack address can escape; atomics become `ATOMIC`; **everything else is `SHARED`**. A real
analyzer refines this (escape/alias/MHP, offline, optionally LLM-assisted) but must keep the same
over-approximation invariant.

## Binary layout (`OMC1`, little-endian)

```
off  size  field
0    4     magic            = 0x31434d4f ("OMC1")
4    2     format_version   = 1
6    2     flags
8    4     isa_id           ISA identity guard (e.g. SM8850)
12   4     analyzer_id      which analyzer produced this
16   4     analyzer_version
20   4     entry_count
24   32    module_hash      SHA-256 of the analyzed guest code
56   ...   entries[entry_count]   each: {u64 guest_pc, u8 mclass, u8 eflags, u16 reserved}  (12 B)
```

Entries are **strictly ascending by `guest_pc`** (binary-search lookup); the parser rejects any
map that isn't. Total length is exactly `56 + entry_count*12` — the parser requires an exact
match (no trailing slack), same discipline as the `OTU1` translation-unit format.

`eflags`: `GUARD_REQUIRED` (LOCAL relaxes only under a runtime guard), `ESCAPED` (was LOCAL,
downgraded to SHARED by escape analysis — informational).

## Identity, keying, and fail-closed retrieval

- **Cache key** = `SHA-256(module_hash ‖ analyzer_id ‖ analyzer_version ‖ isa_id)`. So a map is
  only ever served for the exact module + analyzer + ISA it was built for.
- **On fetch** (`oryx_mmap_cache_get`): the blob's content address is re-verified by
  `liboryxcache` (tamper → `ORYX_ERR_INTEGRITY`), then `oryx_mmap_verify_identity()` re-checks the
  embedded module hash / analyzer version / isa against what was requested. **Any mismatch → the
  map is discarded and the translator falls back to its sound static default** (order everything /
  on-device stack-only relaxation), never to a wrong relaxation.

## How it plugs in

`decide()` returns `RELAX | TSO | ATOMIC`, which maps 1:1 onto the translator's lowering:
`RELAX` → plain weak `LDR/STR`; `TSO` → an ordered access (`liboryxtu`'s `ORYX_ORDER_SC` LDAR/STLR
or `ORYX_ORDER_TSO` DMB-fence — [box64-fex-integration §4](box64-fex-integration.md)); `ATOMIC` →
LSE. The map is the *initial* lowering selector; a runtime promotion (MTE/allocator observing
cross-thread access) flips an access to SHARED and re-translates the block — so a wrong `LOCAL`
guess becomes a re-translate, never a memory-model violation.
