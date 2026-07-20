# Format — tuning profile

Reference implementation: [`src/liboryxprofile`](../../src/liboryxprofile/) (tested).
Examples: [`src/liboryxprofile/examples`](../../src/liboryxprofile/examples/).

A profile is the community-best configuration for **one game build** on **one device class**,
ranked by real telemetry. Simple `key = value` text so it is trivial to crowd-source, diff,
and hand-edit. One profile per `*.profile` file; `#` comments allowed.

## Fields

| Key | Meaning |
|-----|---------|
| `game_build_hash` | identifies the exact game build (its own binaries); a patch gets a new profile |
| `device_class` | e.g. `SM8850` (Snapdragon 8 Elite Gen 5) |
| `requires_hw_tso` | `1` ⇒ only valid on a device where the Oryx driver offers hardware TSO |
| `backend` | `box64` or `fex` |
| `memmodel` | `hw_tso` or `sw_fences` |
| `turnip_build` | GPU driver build id (Part D); pins the pipeline-cache validity |
| `dxvk_opts`, `vkd3d_opts` | D3D→Vulkan translation options |
| `resolution`, `fsr_mode` | render settings (`off`/`quality`/`balanced`/`performance`) |
| `device_spoof` | optional GPU spoof string, empty if none |
| `median_fps`, `p1_low_fps`, `crash_rate`, `sustained_fps` | aggregate opt-in telemetry |
| `sample_count` | sessions behind the numbers (drives confidence shrink) |

## Ranking

`oryx_profile_score()` is deliberately **not** "highest median fps wins":

```
base   = 0.5·p1_low_fps + 0.3·median_fps + 0.2·sustained_fps      (smoothness first)
crash  = shrink(crash_rate toward 0.10 prior, by sample_count)     (unproven ⇒ assume some risk)
conf   = sample_count / (sample_count + 10)                        (undersampled ⇒ discounted)
score  = base · (1 − crash) · (0.5 + 0.5·conf)
```

So a profile that hits 60 fps but crashes often, or is backed by a single lucky session,
ranks below a steady, well-sampled one. This is what makes the crowd data trustworthy.

## Resolution & capability

`oryx_profile_resolve(set, game_build_hash, caps)` returns the highest-scoring profile that
(a) matches the game build, (b) matches the device class, and (c) is compatible with the
device's capabilities — a `hw_tso` profile is only offered when `caps.hw_tso` is set. So the
**same game** resolves to the hardware-TSO profile on a Part-A-capable device and the
software-fence profile on a stock phone, with no user choice.

## Apply

`oryx_profile_apply_env()` renders the profile into GameNative-style `export KEY=VALUE` lines:

- `memmodel = hw_tso` → `ORYX_MM_TSO=1` (the emulator then calls
  [`liboryxmm`](../../src/liboryxmm/)); for FEX also `FEX_TSOENABLED=0` (now safe, since the
  silicon guarantees ordering). No software knobs.
- `memmodel = sw_fences` → `ORYX_MM_TSO=0` plus `BOX64_DYNAREC_STRONGMEM=1` (or
  `FEX_TSOENABLED=1`).
- Plus `ORYX_TURNIP_BUILD`, `DXVK_CONFIG`, `VKD3D_CONFIG`, `ORYX_RES`, `ORYX_FSR`, and
  `ORYX_GPU_SPOOF` when set.

Once Part A ships on a device, the `memmodel`/memory-ordering dimension effectively
disappears from tuning — the hardware handles it — which is the whole point of collapsing
that axis of the manual-tuning burden.
