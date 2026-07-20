# Part D — Curated Turnip driver + precompiled pipeline caches

**Goal:** win the GPU front — which is about driver maturity and shader-compile stutter, not
API-translation throughput.

**Confidence:** builds on Mesa/Turnip **[Established]**; tracks upstream driver maturity.

## Why the GPU front is a *driver* problem

On GPU-bound work, translation overhead is already near-zero (glmark2: Box64 178 vs native
181 fps) because render calls pass straight to the GPU. **[Established]** So the levers are:

1. **Driver quality.** The Adreno 840 (A8xx/"Gen8") is so new that open **Turnip** support
   only lands in Mesa 26.1+/mid-2026; until it matures, the newest 8 Elite phones can emulate
   *worse* than an older 8 Gen 3. The stock proprietary Adreno driver lacks Vulkan extensions
   DXVK needs, so the stack depends on Turnip. Community forks (Kimchi, Mr. Purple, K11MCH1)
   add 30–50% fps by trading strict compliance for speed. **[Established]**
2. **Shader-compilation stutter.** DXVK/VKD3D + Turnip compile pipelines on first use → the
   classic first-traversal hitch.

## Design

### D.1 — Curated per-title Turnip builds

Maintain a small matrix of pinned Turnip builds (stock Mesa release + a couple of vetted
performance forks) with per-game "known-good driver" recommendations feeding Part C's
profile. Pin the build id so Part B's pipeline cache stays valid (a driver change
invalidates compiled pipelines).

### D.2 — Precompiled pipeline-cache delivery

The Turnip pipeline cache and the DXVK/VKD3D SPIR-V cache are deterministic given pinned
versions, so ship them through Part B's CDN keyed by
`(shader_hash ‖ pipeline_state ‖ turnip_build_id)`. The community compiles each pipeline
once; every other player downloads it → the first-traversal stutter is paid collectively,
not per user.

### D.3 — Driver abstraction seam

Expose driver choice as a capability the profile selects, matching the front-ends' existing
"swap Turnip build" affordance — no new user-facing mechanism, just automation and curation.

## Dependency on upstream

Part D's ceiling rises as Adreno 840 Turnip support matures (Mesa 26.1+). Track it; upstream
fixes and profiling data where we can. This part is the most externally-paced of the four.

## Definition of done

- A covered game auto-selects a known-good Turnip build via its profile.
- First launch on a cold device shows no first-traversal shader hitch (pipeline cache
  pre-delivered).
