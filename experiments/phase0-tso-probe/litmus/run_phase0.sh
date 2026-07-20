#!/bin/sh
# run_phase0.sh — the Phase 0 memory-model protocol for Project Oryx.
#
# Runs the MP and SB litmus tests across representative core pairs and prints a
# per-pair verdict plus an overall baseline conclusion. POSIX sh (works in the
# Android shell / Termux). No bashisms.
#
#   Baseline use (Step 1, no TSO bit set): expect every pair to read WEAK on Oryon.
#   Probe use   (Step 2, TSO bit set):     re-run; a pair should flip WEAK -> TSO.
#
# Environment overrides:
#   ITERS=<n>     iterations per measurement (default 50000000)
#   PAIRS="a:b a:b ..."   explicit core pairs, skips auto-detection
#   BIN=./oryx_litmus     path to the built binary
#
# Decision rule (per pair), from the SB sensitivity control + MP witness:
#   SB>0 & MP>0   -> WEAK        (reordering seen, MP-forbidden outcome occurs)
#   SB>0 & MP==0  -> TSO         (harness sensitive, yet MP-forbidden never occurs)
#   SB==0         -> INCONCLUSIVE (harness not exposing reordering; raise ITERS / change pair)
#   MP(fenced)!=0 -> HARNESS BUG (a real barrier must forbid the MP outcome)

set -eu

ITERS="${ITERS:-50000000}"
BIN="${BIN:-./oryx_litmus}"

if [ ! -x "$BIN" ]; then
    echo "building $BIN ..."
    make >/dev/null
fi

# --- extract the "witnesses" field from the tool's --json line ----------------
witnesses() {
    # args: test mode cpu_a cpu_b
    "$BIN" --test "$1" --mode "$2" --iters "$ITERS" --cpu-a "$3" --cpu-b "$4" --json \
        | sed -n 's/.*"witnesses":\([0-9]*\).*/\1/p'
}

# --- choose representative core pairs ----------------------------------------
choose_pairs() {
    if [ -n "${PAIRS:-}" ]; then
        echo "$PAIRS"
        return
    fi
    # Classify cores by max frequency (prime cores clock highest on SM8850).
    # Emit: within-fast, within-slow, and cross-cluster pairs when detectable.
    freqs=""
    for d in /sys/devices/system/cpu/cpu[0-9]*; do
        [ -r "$d/cpufreq/cpuinfo_max_freq" ] || continue
        cpu=$(basename "$d" | sed 's/cpu//')
        f=$(cat "$d/cpufreq/cpuinfo_max_freq" 2>/dev/null || echo 0)
        freqs="$freqs$cpu:$f\n"
    done
    if [ -z "$freqs" ]; then
        echo "0:1 0:4 2:6"          # blind fallback
        return
    fi
    printf "%b" "$freqs" | awk -F: '
        { cpu[NR]=$1; f[$1]=$2; if ($2>maxf){maxf=$2} if(minf==0||$2<minf){minf=$2} }
        END {
            fastlist=""; slowlist="";
            for (c in f) { if (f[c]==maxf) fastlist=fastlist" "c; else slowlist=slowlist" "c }
            split(fastlist, fa, " "); split(slowlist, sl, " ");
            # collect non-empty entries
            nf=0; for(i in fa) if(fa[i]!="") {nf++; F[nf]=fa[i]}
            ns=0; for(i in sl) if(sl[i]!="") {ns++; Sc[ns]=sl[i]}
            out="";
            if (ns>=2) out=out Sc[1]":"Sc[2]" ";      # within perf/slow
            if (nf>=2) out=out F[1]":"F[2]" ";         # within prime/fast
            if (nf>=1 && ns>=1) out=out Sc[1]":"F[1];  # cross cluster
            if (out=="") out="0:1";
            print out;
        }'
}

echo "=================================================================="
echo " Project Oryx — Phase 0 memory-model protocol"
echo " iters/measurement = $ITERS"
echo "=================================================================="

PAIRLIST=$(choose_pairs)
echo "core pairs under test: $PAIRLIST"
echo ""
printf "%-8s %-12s %-12s %-12s %-14s\n" "pair" "SB(relax)" "MP(relax)" "MP(fenced)" "verdict"
printf "%-8s %-12s %-12s %-12s %-14s\n" "----" "---------" "---------" "----------" "-------"

overall_weak=0
overall_tso=0
overall_incon=0

for pair in $PAIRLIST; do
    a=$(echo "$pair" | cut -d: -f1)
    b=$(echo "$pair" | cut -d: -f2)

    sb=$(witnesses sb relaxed "$a" "$b")
    mp=$(witnesses mp relaxed "$a" "$b")
    mpf=$(witnesses mp fenced "$a" "$b")

    verdict="?"
    if [ "$mpf" != "0" ]; then
        verdict="HARNESS-BUG"
    elif [ "$sb" = "0" ]; then
        verdict="INCONCLUSIVE"; overall_incon=$((overall_incon+1))
    elif [ "$mp" = "0" ]; then
        verdict="TSO"; overall_tso=$((overall_tso+1))
    else
        verdict="WEAK"; overall_weak=$((overall_weak+1))
    fi

    printf "%-8s %-12s %-12s %-12s %-14s\n" "$a:$b" "$sb" "$mp" "$mpf" "$verdict"
done

echo ""
echo "------------------------------------------------------------------"
if [ "$overall_tso" -gt 0 ] && [ "$overall_weak" -eq 0 ]; then
    echo " RESULT: TSO in effect on all sensitive pairs."
    echo "         (If this is the BASELINE run with no bit set, the cores are"
    echo "          natively TSO — unexpected on Oryon; double-check the setup."
    echo "          If this is the PROBE run with a candidate bit set: CANDIDATE CONFIRMED.)"
elif [ "$overall_weak" -gt 0 ] && [ "$overall_tso" -eq 0 ]; then
    echo " RESULT: WEAK model on all sensitive pairs (expected Oryon baseline)."
    echo "         Harness is sensitive (SB fired). Proceed to Step 2: scan for the"
    echo "         TSO control bit and re-run this script hoping WEAK -> TSO."
elif [ "$overall_incon" -gt 0 ] && [ "$overall_weak" -eq 0 ] && [ "$overall_tso" -eq 0 ]; then
    echo " RESULT: INCONCLUSIVE — SB never fired; harness not exposing reordering."
    echo "         Raise ITERS, try cross-cluster pairs, or reduce lockstep."
else
    echo " RESULT: MIXED across pairs — inspect the table above pair-by-pair."
fi
echo "------------------------------------------------------------------"
