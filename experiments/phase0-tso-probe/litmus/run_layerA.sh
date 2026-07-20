#!/bin/sh
# run_layerA.sh — Validation-harness Layer A: prove the x86-TSO -> AArch64
# *mapping* is correct on real silicon, with NO root and NO litmus7 install.
#
# Unlike run_phase0.sh (which measures the bare hardware), this lowers the two
# shared litmus accesses using the exact instruction sequences a translator
# emits for a SHARED access, and checks the outcome per mapping:
#
#   plain  STR / LDR                    weak baseline (untranslated)
#   rcpc   STLR / LDAPR                 claim: EXACT x86-TSO
#   sc     STLR / LDAR                  RCsc: STRONGER than TSO (over-strong)
#   dmb    DMB ISHST;STR / LDR;DMB ISHLD  EXACT x86-TSO via fences (pre-8.3)
#
# Two independent signatures decide the run:
#   (1) ORDERING RESTORED — MP-forbidden (r1==1 && r2==0) must be 0 for rcpc,
#       sc and dmb. That is the whole point: the mapping puts back the
#       store->store / load->load ordering weak ARM drops but x86 needs.
#   (2) EXACTNESS — SB-allowed (r0==0 && r1==0) must be NONZERO for rcpc and
#       dmb (they keep the W->R store-buffer relaxation TSO permits) but ZERO
#       for sc. Seeing SB under rcpc while sc suppresses it is the on-device
#       PROOF that LDAPR/STLR is exact-TSO, not accidentally sequential-
#       consistent (which would silently over-cost every SHARED access).
#
# Env overrides:
#   ITERS=<n>          iterations per measurement (default 5000000)
#   PAIR="a:b"         core pair to test (default: auto cross-cluster)
#   BIN=./oryx_litmus  path to the built binary
#
# POSIX sh; no bashisms. Runs in the Android shell / Termux.
set -eu

ITERS="${ITERS:-5000000}"
BIN="${BIN:-./oryx_litmus}"

if [ ! -x "$BIN" ]; then
    echo "building $BIN ..."
    make >/dev/null
fi

# --- pick a cross-cluster pair (widest store-propagation window) -------------
pick_pair() {
    if [ -n "${PAIR:-}" ]; then echo "$PAIR"; return; fi
    freqs=""
    for d in /sys/devices/system/cpu/cpu[0-9]*; do
        [ -r "$d/cpufreq/cpuinfo_max_freq" ] || continue
        cpu=$(basename "$d" | sed 's/cpu//')
        f=$(cat "$d/cpufreq/cpuinfo_max_freq" 2>/dev/null || echo 0)
        freqs="$freqs$cpu:$f\n"
    done
    [ -n "$freqs" ] || { echo "0:4"; return; }
    printf "%b" "$freqs" | awk -F: '
        { f[$1]=$2; if($2>maxf)maxf=$2; if(minf==0||$2<minf)minf=$2 }
        END {
            slow=-1; fast=-1;
            for (c in f) { if (f[c]==minf && slow<0) slow=c; if (f[c]==maxf && fast<0) fast=c }
            if (slow<0) slow=0; if (fast<0) fast=1;
            print slow":"fast;
        }'
}

witnesses() {   # args: test map cpu_a cpu_b
    "$BIN" --test "$1" --map "$2" --iters "$ITERS" --cpu-a "$3" --cpu-b "$4" --json \
        | sed -n 's/.*"witnesses":\([0-9]*\).*/\1/p'
}

PAIR_SEL=$(pick_pair)
A=$(echo "$PAIR_SEL" | cut -d: -f1)
B=$(echo "$PAIR_SEL" | cut -d: -f2)

echo "=================================================================="
echo " Project Oryx — Validation Layer A: TSO->AArch64 mapping check"
echo " core pair = $A:$B   iters/measurement = $ITERS"
echo "=================================================================="
echo ""
printf "%-7s %-14s %-14s %-s\n" "map"   "MP(forbidden)" "SB(allowed)" "reading"
printf "%-7s %-14s %-14s %-s\n" "-----" "-------------" "-----------" "-------"

mp_plain=""; sb_plain=""
mp_rcpc="";  sb_rcpc=""
mp_sc="";    sb_sc=""
mp_dmb="";   sb_dmb=""

for m in plain rcpc sc dmb; do
    mp=$(witnesses mp "$m" "$A" "$B")
    sb=$(witnesses sb "$m" "$A" "$B")
    case "$m" in
        plain) mp_plain=$mp; sb_plain=$sb;;
        rcpc)  mp_rcpc=$mp;  sb_rcpc=$sb;;
        sc)    mp_sc=$mp;    sb_sc=$sb;;
        dmb)   mp_dmb=$mp;   sb_dmb=$sb;;
    esac
    reading=""
    case "$m" in
        plain) reading="weak baseline (both should be > 0)";;
        rcpc)  reading="exact-TSO claim (MP=0, SB>0)";;
        sc)    reading="over-strong (MP=0, SB=0)";;
        dmb)   reading="exact-TSO fence (MP=0, SB>0)";;
    esac
    printf "%-7s %-14s %-14s %-s\n" "$m" "$mp" "$sb" "$reading"
done

echo ""
echo "------------------------------------------------------------------"

# --- verdict --------------------------------------------------------------
gt0() { [ "$1" -gt 0 ] 2>/dev/null; }
eq0() { [ "$1" = "0" ]; }

fail=0

# (0) sanity: the harness must be sensitive on this pair.
if ! gt0 "$sb_plain"; then
    echo " INCONCLUSIVE: plain SB never fired ($sb_plain) — harness not exposing"
    echo "   reordering on pair $A:$B. Raise ITERS or pick a cross-cluster PAIR."
    echo "------------------------------------------------------------------"
    exit 2
fi
if ! gt0 "$mp_plain"; then
    echo " NOTE: plain MP was 0 on $A:$B — this pair did not expose store/load"
    echo "   reordering (looks TSO-ordered already). The mapping checks below are"
    echo "   valid but less informative; try a wider cross-cluster PAIR for (1)."
fi

# (1) ordering restored by every mapping.
for pair in "rcpc:$mp_rcpc" "sc:$mp_sc" "dmb:$mp_dmb"; do
    nm=${pair%%:*}; v=${pair#*:}
    if ! eq0 "$v"; then
        echo " FAIL (1): $nm did NOT restore ordering — MP-forbidden fired ($v). A"
        echo "   correct SHARED mapping must drive MP to 0. This is a real bug."
        fail=1
    fi
done

# (2) exactness signature: rcpc/dmb keep SB (W->R), sc suppresses it.
if gt0 "$sb_rcpc" && gt0 "$sb_dmb"; then
    if eq0 "$sb_sc"; then
        echo " EXACTNESS CONFIRMED: rcpc SB=$sb_rcpc, dmb SB=$sb_dmb (both fire) while"
        echo "   sc SB=0 (suppressed). LDAPR/STLR is exact x86-TSO on this silicon —"
        echo "   it keeps the store-buffer (W->R) outcome TSO allows, and sc is"
        echo "   provably over-strong. This is the empirical proof of the mapping."
    else
        echo " PARTIAL: rcpc/dmb keep SB (good) but sc SB=$sb_sc also fired. Expected"
        echo "   sc to suppress it; sc may be under-fenced on this core, or the pair"
        echo "   is TSO-ordered. Re-run with a wider PAIR / higher ITERS."
    fi
else
    echo " EXACTNESS UNPROVEN: rcpc SB=$sb_rcpc, dmb SB=$sb_dmb — expected both > 0."
    echo "   If sc SB=$sb_sc is also 0 the pair likely isn't exposing W->R; raise"
    echo "   ITERS or choose a cross-cluster PAIR. (rcpc==sc here means the LDAPR"
    echo "   exactness win is simply not observable on this pair, not that it's"
    echo "   wrong.)"
fi

echo "------------------------------------------------------------------"
if [ "$fail" -eq 0 ]; then
    echo " LAYER A: ordering restored by all three mappings."
    echo "          (Exactness line above states whether the LDAPR win was observed.)"
else
    echo " LAYER A: FAILED — a mapping lost ordering (see FAIL lines). Do not ship"
    echo "          that mapping; investigate before trusting weak translation."
fi
echo "------------------------------------------------------------------"
exit "$fail"
