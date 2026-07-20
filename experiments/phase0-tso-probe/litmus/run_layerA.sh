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

ITERS="${ITERS:-20000000}"     # SB needs a wide window; 20M ~ 1-2 min/measurement
BIN="${BIN:-./oryx_litmus}"

if [ ! -x "$BIN" ]; then
    echo "building $BIN ..."
    make >/dev/null
fi

witnesses() {   # args: test map cpu_a cpu_b [iters]
    "$BIN" --test "$1" --map "$2" --iters "${5:-$ITERS}" --cpu-a "$3" --cpu-b "$4" --json \
        | sed -n 's/.*"witnesses":\([0-9]*\).*/\1/p'
}

# --- candidate pairs: auto cross-cluster first, then a spread of fallbacks ----
candidate_pairs() {
    if [ -n "${PAIR:-}" ]; then echo "$PAIR"; return; fi
    # cross-cluster (min-freq : max-freq) from cpufreq, then within-cluster and
    # a few blind fallbacks. Whichever exposes the SB store-buffer first wins.
    freqs=""
    for d in /sys/devices/system/cpu/cpu[0-9]*; do
        [ -r "$d/cpufreq/cpuinfo_max_freq" ] || continue
        cpu=$(basename "$d" | sed 's/cpu//')
        f=$(cat "$d/cpufreq/cpuinfo_max_freq" 2>/dev/null || echo 0)
        freqs="$freqs$cpu:$f\n"
    done
    auto=""
    if [ -n "$freqs" ]; then
        auto=$(printf "%b" "$freqs" | awk -F: '
            { f[$1]=$2; if($2>maxf)maxf=$2; if(minf==0||$2<minf)minf=$2 }
            END {
                slow=-1; fast=-1; fast2=-1;
                for (c in f) {
                    if (f[c]==minf && slow<0) slow=c;
                    if (f[c]==maxf && fast<0) fast=c; else if (f[c]==maxf && fast2<0) fast2=c;
                }
                if (slow<0) slow=0; if (fast<0) fast=1;
                out=slow":"fast;                          # cross-cluster
                if (fast2>=0) out=out" "fast":"fast2;      # within fast cluster
                print out;
            }')
    fi
    # dedup while preserving order; a focused spread (within/cross cluster).
    # Kept short on purpose: on low-yield silicon the scan only needs to find a
    # LIVE pair, not finely rank them — confidence comes from ITERS, not scanning.
    printf "%s 0:1 4:5 6:7 0:6\n" "$auto" \
        | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' '
}

# --- find the MOST sensitive pair (max plain SB), not just the first to fire --
SCAN_ITERS=$(( ITERS / 4 )); [ "$SCAN_ITERS" -lt 2000000 ] && SCAN_ITERS=2000000
A=""; B=""; best_sb=-1
if [ -n "${PAIR:-}" ]; then
    A=$(echo "$PAIR" | cut -d: -f1); B=$(echo "$PAIR" | cut -d: -f2)
else
    echo "scanning core pairs for SB sensitivity (picking the MAX plain SB) ..."
    for p in $(candidate_pairs); do
        pa=$(echo "$p" | cut -d: -f1); pb=$(echo "$p" | cut -d: -f2)
        [ "$pa" = "$pb" ] && continue
        s=$(witnesses sb plain "$pa" "$pb" "$SCAN_ITERS")
        [ -z "$s" ] && s=0
        printf "   pair %-5s plain SB(%s iters) = %s\n" "$pa:$pb" "$SCAN_ITERS" "$s"
        if [ "$s" -gt "$best_sb" ] 2>/dev/null; then best_sb=$s; A=$pa; B=$pb; fi
    done
    echo "   -> most sensitive: $A:$B (plain SB = $best_sb)"
    if [ "$best_sb" -le 0 ] 2>/dev/null; then
        # nothing fired at scan depth; keep the max pair and let full iters try.
        first=$(candidate_pairs | awk '{print $1}')
        A=$(echo "$first" | cut -d: -f1); B=$(echo "$first" | cut -d: -f2)
        echo "   (no pair fired at scan depth; using $A:$B at full iters — raise ITERS if still INCONCLUSIVE)"
    fi
fi

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

# (2) W->R exactness signature: rcpc/dmb keep SB (W->R), sc suppresses it.
#     NOTE: this proves ONLY the store->load (W->R) half of TSO-exactness. The
#     multi-copy-atomicity half (IRIW/WRC) is a SEPARATE property this 2-thread
#     test cannot see — see the caveat printed at the end.
LOWCONF=""
if [ "$sb_plain" -lt 30 ] 2>/dev/null; then
    LOWCONF="yes"
fi
if gt0 "$sb_rcpc" && gt0 "$sb_dmb"; then
    if eq0 "$sb_sc"; then
        echo " W->R EXACTNESS CONFIRMED: rcpc SB=$sb_rcpc, dmb SB=$sb_dmb (both fire) while"
        echo "   sc SB=0 (suppressed). On the store->load axis LDAPR/STLR matches x86-TSO"
        echo "   exactly (keeps the store-buffer outcome TSO allows) and sc is provably"
        echo "   over-strong. This is HALF of exact-TSO; see the MCA caveat below."
        if [ -n "$LOWCONF" ]; then
            echo "   LOW CONFIDENCE: plain SB=$sb_plain is small — the signature is the right"
            echo "   SHAPE but the counts are near the noise floor. Confirm with a long run:"
            echo "       ITERS=500000000 PAIR=$A:$B sh run_layerA.sh"
            echo "   You want plain/rcpc/dmb SB in the tens+ and sc SB still exactly 0."
        fi
    else
        echo " PARTIAL: rcpc/dmb keep SB (good) but sc SB=$sb_sc also fired. Expected"
        echo "   sc to suppress it; sc may be under-fenced on this core, or the pair"
        echo "   is TSO-ordered. Re-run with a wider PAIR / higher ITERS."
    fi
else
    echo " W->R EXACTNESS UNPROVEN: rcpc SB=$sb_rcpc, dmb SB=$sb_dmb — expected both > 0."
    echo "   If sc SB=$sb_sc is also 0 the pair likely isn't exposing W->R; raise"
    echo "   ITERS or choose a cross-cluster PAIR. (rcpc==sc here means the LDAPR"
    echo "   exactness win is simply not observable on this pair, not that it's"
    echo "   wrong.)"
fi

echo "------------------------------------------------------------------"
if [ "$fail" -eq 0 ]; then
    echo " LAYER A: ordering restored by all three mappings (2-thread MP/SB)."
    echo "          W->R exactness line above states whether the LDAPR win was seen."
    echo ""
    echo " MCA CAVEAT (do not skip before shipping rcpc): this 2-thread test does"
    echo "   NOT cover multi-copy atomicity. Whether LDAPR/STLR keeps IRIW and WRC"
    echo "   forbidden (which x86-TSO requires) needs the 3-4-thread tests — run"
    echo "   those via litmus7 next. Until rcpc passes IRIW/WRC on THIS silicon, ship"
    echo "   the sc (LDAR/STLR) or dmb mapping, which are MCA-safe by construction."
else
    echo " LAYER A: FAILED — a mapping lost ordering (see FAIL lines). Do not ship"
    echo "          that mapping; investigate before trusting weak translation."
fi
echo "------------------------------------------------------------------"
exit "$fail"
