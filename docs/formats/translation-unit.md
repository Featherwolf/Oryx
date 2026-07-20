# Format — TranslationUnit (`OTU1`) and the content-addressed store

Reference implementation: [`src/liboryxcache`](../../src/liboryxcache/) (tested).

## Two names for every entry

| Name | What it is | Used for |
|------|-----------|----------|
| **Content address** | `SHA-256(canonical bytes)`, lowercase hex | integrity, dedup, the blob's filename |
| **Logical key** | `SHA-256(guest_hash ‖ translator_id ‖ translator_version ‖ profile_id ‖ isa_id)` | what a client looks up ("do we have a translation for this block, under this config, for this ISA?") |

The store keeps blobs by content address and an `index/<logical_key> → content_address`
mapping. Retrieval always re-hashes the blob and **rejects any mismatch** (`ORYX_ERR_INTEGRITY`)
before the bytes are used — so a corrupted or tampered translation can never be executed.

## TranslationUnit binary layout (little-endian)

```
off  size  field
0    4     magic            = 0x3155544f  ("OTU1")
4    2     format_version   = 1
6    2     reserved         = 0
8    4     isa_id           SM8850-class ISA identity guard
12   4     translator_id    e.g. 1 = box64, 2 = fex
16   4     translator_version
20   4     profile_id       tuning profile assumed by codegen (INCLUDES memory-model variant)
24   4     flags
28   8     guest_entry_pc
36   4     guest_len        bytes of guest code covered by this unit
40   4     code_len         bytes of position-independent AArch64
44   4     reloc_count
48   4     exit_count
52   32    guest_hash       SHA-256 of the guest bytes (source identity)
84   ...   code[code_len]
     ...   relocs[reloc_count]   each: {u32 offset, u32 kind, u64 guest_target}  (16 B)
     ...   exits[exit_count]     each: {u64 guest_pc}                            (8 B)
```

Total length is exactly `84 + code_len + reloc_count*16 + exit_count*8`. The parser
requires an **exact** match (no trailing slack) so the byte stream is canonical and the
content address is deterministic.

### Relocation kinds

| Kind | Meaning |
|------|---------|
| `0` `NONE` | placeholder |
| `1` `ABS64_GUEST_PC` | store the resolved host address of a guest PC at this site |
| `2` `BRANCH_GUEST_PC` | patch a branch at this site to the host address of a guest PC |

Relocations are why a unit is portable: no host addresses are baked in; the runtime resolves
`guest_target`s to host addresses at map time and stitches units together via their `exits`.

## Why portability works here (and not for Rosetta's per-machine cache)

Rosetta keys its AOT cache by binary + local path and never shares it between machines. Oryx
targets a **single fixed ISA** (SM8850), so the same `(guest bytes, translator, profile, isa)`
deterministically yields the same unit on every device — making the logical key portable and
the whole cache shareable. The `isa_id` field guards against ever serving a unit to a
non-identical ISA.

## Memory-model variants are distinct entries

`profile_id` encodes the memory-model assumption. A unit translated for **hardware-TSO-on**
(plain loads/stores) and one for **software ordering** (with barriers) have different
`profile_id`s → different logical keys → separate cache entries. A device is only ever served
the variant its Part A capability supports; the two never mix.

## Determinism requirements (the engineering risk)

For the content address to match across devices, the translator must emit **byte-identical**
`code[]` for identical inputs: pinned register allocation, pinned constant pools, no address-
or time-dependent codegen. The format is deterministic by construction; making Box64/FEX
codegen deterministic is the open work item tracked for Part B implementation.

## Shaders reuse the store

Compiled DXVK/VKD3D SPIR-V and Turnip pipeline caches are stored as **opaque blobs** via the
same `put_blob`/`get_blob` path, keyed by
`SHA-256(shader_hash ‖ pipeline_state ‖ turnip_build_id)`. Same integrity guarantees, same
CDN distribution (Part D).
