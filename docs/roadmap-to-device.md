# Roadmap: from here to a running app on your device

Straight answer to "how many more steps before I can test a running app?" — it depends
entirely on *which* win you want to feel, and those are very different amounts of work. This
document is honest about all three.

## The fact that shapes everything

**Oryx is not an emulator.** It enhances the existing Android stack — GameNative / Winlator +
**Box64/FEX** + Wine + DXVK + Turnip. The reference libraries here (`liboryxtu`, `liboryxcache`,
`liboryxprofile`, `liboryxmm`) are **specs and building blocks**, not a thing you run games on.
So "run a game" = the existing stack; "run a game with an Oryx *win*" = wire one Oryx piece into
that stack. Three tracks, three timelines.

---

## Track A — baseline running app: **testable today, 0 Oryx steps**

Install **GameNative** (or Winlator) on the S26 Ultra and run an x86 Windows game. This is the
"before" that everything else is measured against. Do it first so you have a comparison. No Oryx
code involved.

## Track B — first Oryx win on a running app: **~2–3 steps (config/driver, no emulator surgery)**

The nearest improvement you can actually feel, because it's pure configuration + driver/shader
delivery — no changes to the emulator internals:

1. **Get the tooling on the device.** Build `oryxprofile` and copy it + [`tools/oryx-run.sh`](../tools/oryx-run.sh)
   into Termux (or bundle them). *(Delivered — the launcher works today.)*
2. **Make a real profile for your game.** Measure a good config once (backend, Turnip build,
   DXVK opts, resolution/FSR) and drop it in a `profiles/` dir — or import community settings.
   This is the one content step; `src/liboryxprofile/examples/` shows the format.
3. **Launch through Oryx.** In a **Termux + Box64 + Wine** setup this is literally
   `oryx-run --profiles ./profiles --game <hash> -- box64 wine Game.exe` and you get the tuned
   environment automatically. For **Winlator/GameNative** (GUI apps), "apply the profile" today
   means setting those same values in the app's per-container options — a first-class one-tap
   integration is a future GameNative plugin, not a CLI.

What you'd feel: the right Box64 flags + a known-good Turnip driver + resolution/FSR, applied
automatically instead of hand-tuned — plus (with Part D) precompiled shaders to kill first-run
stutter. **This is the realistic next testable milestone.**

> Honest nuance: the clean CLI path (Termux+Box64+Wine) is real and works now. Winlator/GameNative
> are GUI apps whose launch isn't a shell command, so until a proper plugin exists, the profile is
> applied by hand there. The *engine* that decides the settings is done and tested; the GUI
> integration is packaging.

## Track C — the flagship CPU win on a running app: **weeks, not steps**

The memory-model win (Door 3 / Part A) on a *real game* requires patching **Box64's dynarec** —
this is genuine compiler work, not a packaging step:

1. Fork Box64; add the LOCAL/SHARED/ATOMIC classifier to its x86 decoder + AArch64 emitter
   (`liboryxtu` + [`docs/door3-drf-translation.md`](door3-drf-translation.md) are the blueprint).
2. Thread the memory-model policy through Box64's config and per-game profiles.
3. Build Box64-Oryx for Android; drop it into GameNative in place of stock Box64.
4. Measure vs stock Box64 on a real title.

Effort: multi-week, because Box64's dynarec is a large, mature codebase and correct memory-model
codegen is subtle. This is where the fps/stutter win from ordering actually lands — but it is a
real engineering project. (The rooted-device hardware-TSO path, Part A proper, is a separate
parallel effort gated on Phase 0 Step 2 + a custom kernel.)

---

## Bottom line

| You want to test… | Steps from here | When |
|-------------------|-----------------|------|
| A game running at all (baseline) | 0 | **today** |
| A game with Oryx **auto-tuning** (config/driver/shader) | ~2–3 (tooling delivered; needs a real profile + your launch path) | **near-term** |
| A game with the **memory-model / CPU** win | a multi-week Box64 integration | **later** |

So: you can compare **baseline today**, feel the **auto-tuning win in a couple of packaging
steps**, and the **flagship CPU win is the real build-out** that `liboryxtu` exists to make
tractable. Nothing here is blocked — the tracks just have honestly different sizes.
