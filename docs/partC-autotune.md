# Part C — Crowd-sourced auto-tuning profiles

**Goal:** replace the per-game Discord-thread scavenger hunt with a profile service that
auto-applies the winning configuration on launch.

**Confidence:** pure software **[Established]**; ships first, useful before Part A exists.

## The problem it removes

Getting a demanding title to run well today means hand-assembling: the right backend
(Box64 vs FEX), the right memory-model setting, the right Turnip build, DXVK/VKD3D options,
resolution/upscaling (FSR), and sometimes a GPU "device-spoof" trick. That knowledge is
scattered and re-derived by every user. **[Established — AYN Odin 3 / 8 Elite field reports]**

## Design

A profile is keyed by **game build hash** (so a game patch gets its own profile) and,
secondarily, by device class (SM8850) and Oryx capability level (HW-TSO available or not).

```
Profile {
  game_build_hash
  device_class            // SM8850
  capability              // { hw_tso: bool, turnip_build: id }
  backend                 // box64 | fex
  memory_model            // hw_tso | sw_fences   (auto: hw_tso if capability.hw_tso)
  dxvk_opts, vkd3d_opts
  render { resolution, fsr_mode, ... }
  driver_build            // Part D selection
  score { median_fps, p1_low_fps, crash_rate, thermal }   // telemetry-ranked
}
```

- **Ranking:** profiles are ranked by crowd telemetry — median fps, 1%-low, crash rate, and
  sustained (thermal) fps, not just peak. A profile that hits 60 fps then thermal-throttles
  to 30 ranks below a steady 50.
- **Application:** on launch, Oryx fetches the top-ranked profile for `(game_build_hash,
  device_class, capability)` and populates the front-end's existing per-game config.
- **Exploration:** a small fraction of sessions try candidate variations (bandit-style) to
  keep improving profiles as drivers and translators update.
- **Privacy:** telemetry is opt-in, aggregate, and carries no personal content — only
  config + performance counters keyed by game hash.

## Why it ships first

Part C is independent of the hardware bet and delivers immediate value: even with today's
software ordering, most users are leaving performance on the table through misconfiguration.
Once Part A lands, the **memory_model** dimension largely collapses (hardware handles it), so
profiles get simpler and more robust.

## Definition of done

- On a fresh install, launching a covered game auto-applies a profile and reaches
  community-best performance with zero manual tuning.
- Profiles measurably improve over time as telemetry accumulates.
