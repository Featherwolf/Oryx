# Testing Project Oryx

There are three tiers, from zero-effort to the real on-device experiment. Start at the top.

---

## Tier 1 — Automatic CI (nothing to install) ✅

Every push runs the full host test suite **and** a cross-compile for the S26 Ultra's
architecture, on GitHub's servers. You just watch the result — great from the GitHub mobile
app.

**How:**
1. Open the repo → **Actions** tab (in the GitHub app: repo → the **⚙️/Actions** section).
2. Look at the latest **CI** run. Two jobs should show green checks:
   - **Host build + unit tests** — compiles and runs all 84 unit assertions.
   - **aarch64 cross-build** — proves the whole tree compiles for the Snapdragon 8 Elite
     Gen 5 (arm64), including the real `DMB ISH` memory-barrier path.

A green check means everything builds and every test passes. Nothing to run yourself.

---

## Tier 2 — Local host test (any Linux or Mac, ~30 seconds)

Runs the exact same suites on your own machine. Needs only a C compiler (`cc`/`gcc`/`clang`)
and `make` — no phone, no root, no dependencies.

```sh
git clone https://github.com/Featherwolf/Oryx
cd Oryx
make check
```

**Expected output (the important lines):**

```
== check liboryxmm ==
hardware TSO unavailable -> keep SOFTWARE memory-model emulation
== check liboryxcache ==
28 passed, 0 failed
== check liboryxtu ==
29 passed, 0 failed
== check liboryxprofile ==
27 passed, 0 failed
All Oryx userspace test suites passed.

== Phase 0 litmus self-test (host) ==
  SB ... witnesses = <nonzero>      <- detector is sensitive
  MP relaxed ... witnesses = 0      <- your x86/ARM host is TSO -> correct
  MP fenced  ... witnesses = 0
```

That last block is the memory-model detector proving itself: on a normal x86 machine (which
*is* Total Store Order) it correctly reports the store-buffer control firing while the
message-passing test stays at zero — i.e. it identifies a strong-ordered core correctly. On a
weakly-ordered ARM device with no TSO bit set, the message-passing test would instead be
**nonzero** (WEAK).

**What each suite proves:**

| Suite | What it verifies |
|-------|------------------|
| `liboryxmm` | The hardware-TSO client **fails closed** — with no driver, it falls back to software ordering (the safe default). |
| `liboryxcache` | The translation/shader cache: format round-trip, deterministic content addressing, and **tamper rejection** (a corrupted blob is refused before use). |
| `liboryxtu` | The deterministic translator emits **real, correct AArch64** (known-answer encodings), is byte-identical across runs, and round-trips through the cache. |
| `liboryxprofile` | Auto-tuning: telemetry-aware ranking and **capability-aware resolution** (a hardware-TSO profile is only chosen on a device that has it). |

You can also prove the target build yourself:

```sh
sudo apt-get install -y gcc-aarch64-linux-gnu   # Debian/Ubuntu
make aarch64                                     # cross-compiles the whole tree for arm64
```

---

## Tier 3 — The real experiment on the Galaxy S26 Ultra (Phase 0)

This is the scientifically meaningful test: it measures whether the S26 Ultra's Oryon cores
are weakly ordered at baseline (expected), and is the harness that later confirms whether the
hardware TSO bit can be flipped (the whole project's gate). **No root needed for the baseline
measurement.**

### Option A — Termux (easiest, no PC)

1. Install **Termux** (from F-Droid) on the phone.
2. In Termux:
   ```sh
   pkg install git clang make
   git clone https://github.com/Featherwolf/Oryx
   cd Oryx/experiments/phase0-tso-probe/litmus
   make
   sh run_phase0.sh
   ```
3. Read the verdict table. **Expected on a stock S26 Ultra: `WEAK` on the sensitive core
   pairs** (with the Store-Buffer control firing). That is the baseline the project predicts
   and the starting point Part A aims to change.

### Option B — from a PC over ADB

```sh
# with the Android NDK installed and the phone in USB-debug mode:
cd experiments/phase0-tso-probe/litmus
make NDK=/path/to/android-ndk API=34
adb push oryx_litmus run_phase0.sh /data/local/tmp/
adb shell "cd /data/local/tmp && sh run_phase0.sh"
```

### Reading the result

| Verdict | Meaning |
|---------|---------|
| **WEAK** | Cores are weakly ordered (expected baseline). The detector is working; this is what motivates Part A. |
| **TSO** | Cores behave as Total Store Order on that pair. On a stock phone this would be surprising — recheck the setup. |
| **INCONCLUSIVE** | The Store-Buffer control didn't fire — raise `ITERS` or try cross-cluster core pairs (see `litmus/README.md`). |

The **Step 2** control-bit hunt (`experiments/phase0-tso-probe/probe/`) is the next stage and
**requires a rooted, throwaway test device** — it pokes privileged CPU registers and can crash
or brick a phone. Do not run it on a daily driver. See that directory's README first.

---

## What is *not* yet runnable end-to-end

Honest scope: the kernel modules (`experiments/.../probe`, `src/kmod/oryx_memmodel`) build
against a target-device kernel but are not wired into a running Box64/FEX yet, and Part A's
performance win is gated on the Phase 0 Step 2 result. Tiers 1–2 fully exercise the userspace
libraries; Tier 3 runs the baseline experiment. Everything claimed as "built & tested" in the
README is covered by Tiers 1–2.
