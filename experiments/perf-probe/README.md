# oryx-perf — find the *real* bottleneck before optimizing anything

The honest question for "why are my games slow on the S26 Ultra" is **what is
actually the limiter** — the x86→ARM CPU emulation, the GPU/driver, or thermals?
Effort spent on the wrong one is wasted. This is a **no-root** on-device probe
that samples the resources that gate frame rate while you play, and prints a
**bottleneck verdict**. Use it to target facts, not vibes.

> TL;DR from research on this device: the Snapdragon 8 Elite's Adreno GPU is **not
> yet supported by the fast Turnip driver** (Winlator/GameNative fall back to the
> experimental **Vortek** driver), so most modern games are expected to be
> **GPU/driver-bound**, and CPU-emulation work won't raise their FPS. This tool
> is how you *confirm that on your hardware, per game*, instead of guessing.

## Build (Termux, no root, no PC)

```sh
pkg install git clang make
git clone https://github.com/Featherwolf/Oryx
cd Oryx/experiments/perf-probe
make
```

## Measure a game (two signals, together)

**1. Turn on the GPU HUD in your Wine/Box64 environment.** In GameNative/Winlator,
add these to the container/game **environment variables** (Settings → Environment
Variables, or the per-game env):

```
DXVK_HUD=fps,frametimes,gpuload,version
```

`gpuload` is the key number: **if it sits near 100%, you are GPU-bound** and no
CPU-side change will help. `frametimes` shows stutter (spikes = frame-pacing
problems). Note the FPS it reports.

**2. Run the probe from Termux while the game runs.** Start a demanding scene,
then in a Termux session:

```sh
./oryx-perf --secs 60 --hz 4 --csv run.csv --fps 45
#   --secs   how long to sample (play a heavy scene during this)
#   --hz     samples/second (4 is plenty)
#   --csv    save the time series (per-core util/freq, GPU, temp, memory)
#   --fps    the FPS DXVK_HUD showed (optional; sharpens the verdict)
```

It prints a live line and, on exit, a verdict.

## Reading the verdict

| Verdict | What it means | Where the win is |
|---------|---------------|------------------|
| **GPU / DRIVER BOUND** | GPU pinned ~100%; CPU has headroom | GPU driver (Vortek vs Turnip), DXVK/Zink settings, in-game resolution/quality. **CPU-emulation work will not help.** |
| **CPU / EMULATION BOUND** | One core pinned ~100%; GPU not saturated | Box64/FEX tuning (`BOX64_DYNAREC_*`), preloaded code cache, pin the game to a Prime core. This is where an emulator improvement *could* matter. |
| **THERMAL THROTTLED** | CPU clocks fall as temps rise | Cooling, power profile, lower settings — it's heat, not compute. |
| **LIKELY GPU BOUND (inferred)** | Low FPS, no core maxed, GPU counter unreadable | Confirm with the `gpuload` HUD number. |
| **MIXED / INCONCLUSIVE** | Nothing clearly pinned | Re-run on a heavier scene; add `--fps`; enable `gpuload`. |

## Honest limitations (read these)

- **GPU counters may read `n/a`.** Adreno's kgsl sysfs (`gpu_busy_percentage`,
  `gpuclk`) is often SELinux-locked without root. That's why **DXVK_HUD `gpuload`
  is the primary GPU signal** — the probe's CPU/thermal data plus that HUD number
  give a solid picture together.
- **"A core is pinned" ≠ always CPU-bound.** Box64/Wine spread work across cores;
  the emulator's hot thread is usually the tallest, but a game that is genuinely
  GPU-bound can still show one busy core. Cross-check against `gpuload`.
- **One scene, one verdict.** Different scenes bottleneck differently (a menu is
  GPU-trivial; a busy firefight is not). Sample the *demanding* moments.
- This measures; it doesn't fix. Its job is to point the next effort at the
  resource that's actually limiting *your* games on *your* device.
