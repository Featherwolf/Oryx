# Phase 0 Step 2 — runbook (find Oryon's TSO control bit)

The payoff experiment: hunt for a CPU register bit that, when set, flips a core pair from
**WEAK → TSO** (i.e. drives `MP(relax)` to 0 while `SB` keeps firing) — direct evidence of a
Rosetta-style hardware x86 memory mode on Oryon. That result is the gate for Part A.

> ⚠️ **This can brick a phone.** It writes IMPLEMENTATION DEFINED EL1 CPU registers. Use a
> **dedicated, throwaway, bootloader-unlocked** test device with a known recovery path
> (Samsung Odin / EDL). **Never a daily driver.** Baseline Step 1 (the litmus in `../litmus/`)
> needs no root; only this step does.

Baseline is already established: on the S26 Ultra all pairs read **WEAK** with a sensitive
detector (see [`RESULTS.md`](../../../RESULTS.md)). That's the precondition — now we look for
the flip.

---

## Prerequisites

1. **Throwaway S26 Ultra** (or another SM8850 device), **bootloader unlocked**, **rooted**
   (Magisk). Accept it may need reflashing.
2. A recovery path proven to work **before** you start (Odin + stock firmware, or EDL).
3. A host PC with `adb`, an `aarch64` cross-toolchain, and the **matching kernel source**
   (the hard part — see next section).

## The hard part: building a module for *this* kernel

A kernel module must match the running kernel (version + config + `vermagic`). Two routes:

- **Samsung Open Source (recommended).** Samsung publishes kernel source per device/build at
  <https://opensource.samsung.com>. Download the source matching your exact build number
  (Settings → About phone → Software information → Kernel version / Build number), configure
  it to match the running kernel, and build the module against it.
- **GKI headers.** The S26 Ultra uses Android's Generic Kernel Image; if you can obtain the
  GKI headers/`Module.symvers` for the exact running build, you can build a loadable module
  without the full source. Vermagic must match or `insmod` rejects it.

Build:

```sh
cd experiments/phase0-tso-probe/probe
make KDIR=/path/to/s26-kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

If `insmod` later fails with "version magic" or "disagrees about version of symbol", the
kernel tree doesn't match the device build — fix that before continuing.

## Run the hunt

```sh
# push tool + module + the litmus binary
adb push oryx_probe.ko probe_scan.sh /data/local/tmp/
adb push ../litmus/oryx_litmus /data/local/tmp/

adb shell su -c 'insmod /data/local/tmp/oryx_probe.ko'

# scan on a pair that showed strong reordering at baseline.
# 0:6 (cross-cluster) was strongest; with root, affinity pinning works properly.
adb shell su -c 'cd /data/local/tmp && LITMUS=./oryx_litmus sh probe_scan.sh 0 6'

adb shell su -c 'cat /sys/kernel/debug/oryx_probe/log'
adb shell su -c 'rmmod oryx_probe'
```

`probe_scan.sh` first re-confirms the baseline (WEAK, SB firing), then sweeps each candidate
`(register, bit)`: it sets the bit, runs MP + SB pinned to the pair, clears it, and flags any
bit that produced **WEAK → TSO** while SB stayed nonzero.

## Reading the outcome

| What you see | Meaning | Next |
|--------------|---------|------|
| A `(reg, bit)` flips **WEAK → TSO** (MP→0, SB stays >0) | **The hardware TSO bit.** 🎯 | Characterize it (below), then wire into the production driver |
| No bit flips it | The candidate register set didn't include it | Refine encodings in `oryx_probe.c` and re-sweep (below) |
| SB drops to 0 when a bit is set | That bit broke coherence/perf, not a TSO win | Discard; it's not the mode |
| Device hangs / reboots | A write hit something dangerous | Hard-reboot; recovery if needed; narrow the sweep |

### If you find the bit — characterize it

1. **Scope:** set it on one core; does another core still reorder? (per-core vs global)
2. **Core restriction:** does it work on Prime cores, Performance cores, or both? (Apple
   restricts TSO threads to performance cores.)
3. **Cost:** run a throughput microbenchmark with it on vs off (Apple's hardware TSO costs
   ~8.9%).
4. **Stability:** soak under load; confirm no corruption.

Then feed the discovered values into the production driver
[`src/kmod/oryx_memmodel`](../../../src/kmod/oryx_memmodel/):

```sh
insmod oryx_memmodel.ko oryx_reg_idx=<N> oryx_bit=<B> oryx_confirmed=1 oryx_eligible=<mask>
```

That turns the finding into the per-thread hardware-TSO grant Box64/FEX consume via
`liboryxmm` — i.e. Part A becomes real.

### If no bit flips it

The candidate registers in `oryx_probe.c` (`ACTLR_EL1`, `AIDR_EL1`, a few `S3_0_C15_*`) are a
starting hypothesis, not the answer. To go deeper:

- Add more IMPDEF encodings to the candidate table (compare against how Windows-on-ARM/Prism
  configures Oryon, and against Apple's `ACTLR_EL1` bit layout).
- Widen `REGS`/`BITS` in `probe_scan.sh`.
- Check `AIDR_EL1` for a feature-advertisement bit first (Apple advertises TSO support there).
- If an exhaustive, careful sweep finds nothing, the honest conclusion is that Oryon exposes
  no OS-toggleable per-core TSO on this firmware — and Part A falls back to the Lasagne-style
  optimized-fence software floor (documented in `docs/partA-hardware-tso.md`).

## Safety checklist

- [ ] Throwaway device, bootloader unlocked, **recovery proven before starting**
- [ ] Not your daily driver
- [ ] Module built against the **exact** device kernel (insmod succeeds cleanly)
- [ ] `rmmod` restores state; reboot clears any register writes (they are not persistent)
- [ ] Keep Odin/EDL + stock firmware ready in case a write wedges the device
