#!/bin/sh
# probe_scan.sh — Step 2 orchestrator: hunt Oryon's memory-ordering control bit.
#
# For each candidate (register, bit), set it on a target core via the oryx_probe
# module, then run the MP + SB litmus tests pinned to that core. Report any
# (register, bit) that flips the pair from WEAK to TSO while SB keeps firing —
# that is the signature of the hardware x86 memory mode.
#
# ⚠  Requires: rooted test-only device, oryx_probe.ko loaded, oryx_litmus built.
#
#   insmod oryx_probe.ko
#   sh probe_scan.sh <target_cpu> <peer_cpu>
#
# Env: ITERS (default 20000000), REGS (candidate indices, default "0 2 3 4 5"),
#      BITS (default "0 1 2 3 4 5 6 7"), LITMUS (path to oryx_litmus).

set -eu

CPU_A="${1:-6}"     # target core whose register we poke + pin thread 0
CPU_B="${2:-7}"     # peer core for thread 1
ITERS="${ITERS:-20000000}"
REGS="${REGS:-0 2 3 4 5}"           # skip idx 1 (AIDR_EL1, read-only)
BITS="${BITS:-0 1 2 3 4 5 6 7}"
LITMUS="${LITMUS:-../litmus/oryx_litmus}"
DBG=/sys/kernel/debug/oryx_probe

[ -w "$DBG/control" ] || { echo "oryx_probe not loaded (no $DBG)"; exit 1; }
[ -x "$LITMUS" ] || { echo "build ../litmus/oryx_litmus first"; exit 1; }

wit() { # test cpu_a cpu_b -> witnesses
    "$LITMUS" --test "$1" --mode relaxed --iters "$ITERS" \
        --cpu-a "$2" --cpu-b "$3" --json \
        | sed -n 's/.*"witnesses":\([0-9]*\).*/\1/p'
}

poke() { echo "poke $1 $2 $CPU_A $3" > "$DBG/control"; }

echo "=================================================================="
echo " Oryx Phase 0 Step 2 — control-bit scan"
echo " target cpu=$CPU_A peer cpu=$CPU_B iters=$ITERS"
echo "=================================================================="

# Baseline (nothing poked): confirm WEAK + SB sensitive before trusting flips.
base_sb=$(wit sb "$CPU_A" "$CPU_B")
base_mp=$(wit mp "$CPU_A" "$CPU_B")
echo "baseline: SB=$base_sb MP=$base_mp"
if [ "$base_sb" = "0" ]; then
    echo "ABORT: SB=0 at baseline — harness not sensitive on this pair."
    echo "       Raise ITERS or pick a cross-cluster pair before scanning."
    exit 2
fi
if [ "$base_mp" = "0" ]; then
    echo "NOTE: MP already 0 at baseline — pair looks TSO before poking."
    echo "      Either these cores are natively strong, or a bit is already set."
fi
echo ""
printf "%-16s %-4s %-10s %-10s %s\n" "register" "bit" "SB" "MP" "result"
printf "%-16s %-4s %-10s %-10s %s\n" "--------" "---" "--" "--" "------"

found=""
for r in $REGS; do
    for b in $BITS; do
        # set the bit, measure
        if ! poke "$r" "$b" 1 2>/dev/null; then
            continue   # register not writable / rejected
        fi
        sb=$(wit sb "$CPU_A" "$CPU_B")
        mp=$(wit mp "$CPU_A" "$CPU_B")
        # clear it again to isolate each bit
        poke "$r" "$b" 0 2>/dev/null || true

        result="-"
        if [ "$sb" != "0" ] && [ "$mp" = "0" ] && [ "$base_mp" != "0" ]; then
            result="*** WEAK->TSO — CANDIDATE ***"
            found="$found reg$r:bit$b"
        fi
        printf "%-16s %-4s %-10s %-10s %s\n" "idx$r" "$b" "$sb" "$mp" "$result"
    done
done

echo ""
if [ -n "$found" ]; then
    echo "CANDIDATE CONTROL BIT(S) FOUND:$found"
    echo "Next: characterize scope/core-restriction/cost, then wire into the"
    echo "production module (src/kmod/oryx_memmodel) as the discovered (reg,bit)."
else
    echo "No bit flipped WEAK->TSO. Widen REGS/BITS, refine candidate encodings"
    echo "in oryx_probe.c, or conclude Oryon exposes no per-core TSO toggle here."
fi
