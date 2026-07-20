# Part B — Portable translation & shader cache ("translation CDN")

**Goal:** never translate the same x86 block or compile the same shader twice, on *any*
S26 Ultra. Kill cold-start JIT stutter and shader-compilation hitching by sharing the warmed
cache across the whole install base.

**Confidence:** caching model proven elsewhere (Rosetta AOT, Steam shader cache)
**[Established]**; deterministic DBT codegen is the engineering risk **[R&D]**.

## The enabling insight

Every S26 Ultra is ISA-identical (SM8850). A translation unit or compiled shader produced on
one device is byte-valid on all of them (see [ADR-0002](adr/0002-content-addressed-portable-cache.md)).
Rosetta caches translations per-machine; we cache them **per-ISA, across machines.**

## B.1 — Deterministic, relocatable translation units

Today Box64/FEX JIT into process-local buffers with embedded absolute addresses — neither
deterministic nor portable. To make output cacheable we need each translation unit (TU) to be:

- **Content-addressed:** keyed by `hash(guest_bytes ‖ translator_version ‖ profile_id ‖ isa_id)`.
- **Position-independent:** no baked-in host addresses; all guest→host targets go through a
  relocation table resolved at map time.
- **Deterministic:** identical inputs ⇒ identical bytes. Requires pinning register allocation,
  constant pools, and any RNG/address-dependent codegen. (This is the hard part; it is a
  known-tractable compiler problem, not research.)
- **Self-describing:** carries its guest entry PC, guest byte range, relocation table,
  and the exit edges (direct/indirect) so the runtime can stitch TUs together.

```
TranslationUnit {
  magic, format_version
  isa_id                       // SM8850-class guard
  translator_id + version
  profile_id                   // tuning profile the codegen assumed
  guest_entry_pc, guest_len
  guest_hash (SHA-256)         // of the guest bytes, for integrity
  code[]                       // position-independent AArch64
  relocs[]                     // {offset, kind, guest_target}
  exits[]                      // successor guest PCs (for prefetch)
}
```

## B.2 — Shader cache

DXVK/VKD3D compile D3D shaders to SPIR-V; Turnip compiles SPIR-V + pipeline state to Adreno
ISA. Both are deterministic given pinned versions. Cache both layers:

- **SPIR-V layer** keyed by D3D shader hash + DXVK/VKD3D version.
- **Pipeline layer** keyed by SPIR-V hash + pipeline state + **Turnip build id** (Part D
  pins the build).

This is the Steam shader-precache idea, delivered per-game through the same CDN.

## B.3 — Distribution

```
 first player        Oryx CDN (content-addressed store)        every other player
 ───────────         ─────────────────────────────────        ──────────────────
 warm TUs + shaders  →  verify + sign + dedup by hash  →  bundle per game build  →  download
                                                                                 →  verify hash
                                                                                 →  mmap (RX) TUs,
                                                                                    load pipeline cache
```

- **Integrity:** the client verifies each unit's SHA-256 and signature **before** mapping any
  page executable. No unverified bytes ever become code.
- **Bundling:** cache is packaged per *game build hash* (the game's own binaries), so a patch
  that changes the executable naturally invalidates only the affected TUs.
- **Warm sources:** organic (real players) plus an **off-device warming farm** that can run
  slower/better translation — including LLM-assisted static translation of static hot paths
  (EuroSys'26) — and publish the results as ordinary cache entries.

## B.4 — What still needs the on-device JIT

Static caching cannot cover self-modifying code, runtime script JITs, anti-cheat, or
DRM-unpacked code. The JIT stays; the cache simply removes the *repeat* cost of everything
that is stable. On a second launch (or a first launch of a popular game) the JIT should be
nearly idle.

## Interaction with Part A

The `profile_id` in the cache key includes the memory-model setting. TUs translated for
**hardware-TSO-on** (plain loads/stores) differ from **software-ordering** TUs (with
barriers) — they are distinct cache entries and never mixed. A device gets the variant its
Part A capability supports.

## Definition of done

- Box64 (and/or FEX) emit deterministic, relocatable TUs; two devices produce byte-identical
  output for the same input.
- A popular game's first launch on a cold device pulls a warmed cache and shows no
  first-traversal shader hitch and no JIT warmup spikes.
- Cache integrity: a tampered unit is rejected before execution (tested).
