# ADR-0002 — Content-addressed, portable translation & shader cache

- **Status:** Accepted
- **Date:** design phase

## Context

Two of the most *felt* problems on Android are cold-start JIT stutter (hot code is
translated the first time it's reached) and shader-compilation hitching (DXVK/VKD3D compile
pipelines on first use). Rosetta already solved the translation half *for one machine*: the
`oahd` daemon AOT-translates binaries and caches them under `/var/db/oah/`, keyed by a
SHA-256 of the binary contents + path, written atomically (`.aot.in_progress` → rename).
**[Established — Champollion RE; Mandiant]** But that cache never leaves the device.

We have a lever Apple's general-purpose OS does not: **a single fixed hardware target.**
Every S26 Ultra has the identical SM8850 ISA, so a translation unit produced on one device
is byte-valid on every other. The same holds for compiled SPIR-V shaders against a pinned
Turnip build.

## Decision

Make Box64/FEX translation output and DXVK/VKD3D+Turnip shader output **deterministic and
content-addressed**, then distribute the cache across devices ("translation CDN").

Cache key = `hash(guest_block_bytes ‖ translator_version ‖ tuning_profile_id ‖ isa_id)`.
The first player to run a game warms its blocks and shaders; everyone else downloads
pre-translated ARM code and precompiled shaders and pays neither JIT warmup nor shader hitch.

## Consequences

- **Positive:** Network effect — the system gets better as more people play. First-launch
  experience approaches "already warmed."
- **Positive:** Expensive/better translation can run **off-device** (including LLM-assisted
  static translation of static hot paths) and simply be shipped as cache entries.
- **Negative:** Requires making the DBTs emit **relocatable, position-independent,
  deterministic** translation units — today they are neither deterministic nor portable.
  This is the core engineering risk of Part B.
- **Negative:** Cache poisoning / integrity is a security surface → every unit is
  content-addressed and signed; the client verifies the hash before mapping executable code.
- **Negative:** The on-device JIT must remain, to cover self-modifying / dynamically
  generated code that no static cache can hold.

## Notes

`isa_id` in the key allows the cache to extend to future ISA-identical devices without
collision; a translation warmed on an SM8850 is only served to SM8850-class ISA IDs.
