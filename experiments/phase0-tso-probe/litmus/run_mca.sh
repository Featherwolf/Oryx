#!/bin/sh
# run_mca.sh — HARDWARE multi-copy-atomicity SANITY CHECK (3-thread WRC).
#
# IMPORTANT framing (settled by adversarial review, see docs/validation-harness.md):
# this is NOT an "rcpc gate". WRC/IRIW canNOT distinguish rcpc (LDAPR) from sc
# (LDAR): the only thing RCpc relaxes vs RCsc is the store-release -> load-acquire
# (STLR->LDAPR, W->R) edge, and WRC/IRIW contain no such edge. In WRC, LDAPR is a
# plain acquire — it orders T1's load->store and T2's load->load exactly like LDAR.
# So theory predicts rcpc == sc == dmb == 0 (all forbid WRC), guaranteed by ARMv8
# being other-multi-copy-atomic. The rcpc-vs-sc difference is ENTIRELY the SB test
# in run_layerA.sh; that is what actually clears rcpc as exact-TSO.
#
# What this script IS good for: confirming this silicon really behaves
# multi-copy-atomically under the ordered mappings (a property BOTH rcpc and sc
# rely on). If an ordered mapping ever let WRC through, that would be a hardware
# MCA anomaly or a harness artifact — worth knowing — not an rcpc-mapping bug.
#
#   WRC:  T0: x=1     T1: r1=x; y=1     T2: r2=y; r3=x
#   witness = r1==1 && r2==1 && r3==0   (non-multi-copy-atomic; forbidden on an MCA
#   machine once the reader loads are ordered). plain (unordered LDR) can fire it
#   via reader load-load reordering = the sensitivity control that the harness is
#   live on this core triple.
#
# Env: ITERS (default 20000000), CPUS="a b c" (force a triple), BIN (./oryx_litmus).
# POSIX sh; runs in Termux / the Android shell.
set -eu

ITERS="${ITERS:-20000000}"
BIN="${BIN:-./oryx_litmus}"
[ -x "$BIN" ] || { echo "building $BIN ..."; make >/dev/null; }

# core count, to reject triples that would fail to pin (silent unpinned = invalid)
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 0)

wrc() {   # args: map a b c [iters]
    "$BIN" --test wrc --map "$1" --iters "${6:-$ITERS}" \
        --cpu-a "$2" --cpu-b "$3" --cpu-c "$4" --json \
        | sed -n 's/.*"witnesses":\([0-9]*\).*/\1/p'
}

# reasonable "the harness is actually sensitive" floor — one stray witness is not
# evidence of anything (adversarial review flagged gt0-accepts-1 as too weak).
SENSE_MIN=8

TRIPLES="0 3 6|1 4 7|0 4 6|2 5 7|0 1 2"
[ -n "${CPUS:-}" ] && TRIPLES="$CPUS"

SCAN_ITERS=$(( ITERS / 4 )); [ "$SCAN_ITERS" -lt 3000000 ] && SCAN_ITERS=3000000
A=""; B=""; C=""; best=-1
echo "scanning core triples for WRC sensitivity (max plain WRC) ..."
OIFS=$IFS; IFS='|'
for t in $TRIPLES; do
    IFS=' ' read -r ta tb tc _ <<EOF
$t
EOF
    [ -n "${tc:-}" ] || continue
    # skip triples that reference a core this machine doesn't have (would run
    # unpinned and silently share a core -> invalid measurement)
    if [ "$NCPU" -gt 0 ] 2>/dev/null; then
        if [ "$ta" -ge "$NCPU" ] || [ "$tb" -ge "$NCPU" ] || [ "$tc" -ge "$NCPU" ]; then
            printf "   triple %-8s skipped (needs core >= nproc=%s)\n" "$ta,$tb,$tc" "$NCPU"
            continue
        fi
    fi
    w=$(wrc plain "$ta" "$tb" "$tc" "$SCAN_ITERS"); [ -z "$w" ] && w=0
    printf "   triple %-8s plain WRC(%s iters) = %s\n" "$ta,$tb,$tc" "$SCAN_ITERS" "$w"
    if [ "$w" -gt "$best" ] 2>/dev/null; then best=$w; A=$ta; B=$tb; C=$tc; fi
done
IFS=$OIFS
[ -n "$A" ] || { A=0; B=1; C=2; }
echo "   -> using triple $A,$B,$C (plain WRC = $best)"

echo "=================================================================="
echo " Project Oryx — hardware MCA sanity check (WRC forbidden under ordered maps?)"
echo " cores = $A,$B,$C   iters/measurement = $ITERS"
echo " NOTE: this does NOT distinguish rcpc from sc (identical on WRC). The"
echo "       rcpc-vs-sc discriminator is SB — see run_layerA.sh."
echo "=================================================================="
echo ""
printf "%-7s %-16s %s\n" "map"   "WRC(forbidden)" "reading"
printf "%-7s %-16s %s\n" "-----" "--------------" "-------"

w_plain=$(wrc plain "$A" "$B" "$C"); w_rcpc=$(wrc rcpc "$A" "$B" "$C")
w_sc=$(wrc sc "$A" "$B" "$C");       w_dmb=$(wrc dmb "$A" "$B" "$C")
for x in "plain:$w_plain:sensitivity control (should fire on weak ARM)" \
         "rcpc:$w_rcpc:ordered — expect 0 (same as sc; not a discriminator)" \
         "sc:$w_sc:ordered — expect 0" \
         "dmb:$w_dmb:ordered — expect 0"; do
    m=$(echo "$x" | cut -d: -f1); v=$(echo "$x" | cut -d: -f2); r=$(echo "$x" | cut -d: -f3-)
    printf "%-7s %-16s %s\n" "$m" "$v" "$r"
done

echo ""
echo "------------------------------------------------------------------"
eq0() { [ "$1" = "0" ]; }
ge_sense() { [ "$1" -ge "$SENSE_MIN" ] 2>/dev/null; }

if ! ge_sense "$w_plain"; then
    echo " INCONCLUSIVE: plain WRC = $w_plain (< $SENSE_MIN) — the lockstep harness barely"
    echo "   exposed the outcome on cores $A,$B,$C (WRC yield is intrinsically low). Nothing"
    echo "   about the ordered mappings can be concluded from this triple. Try CPUS=\"0 4 7\","
    echo "   raise ITERS, or use litmus7 (engineered for high WRC yield)."
    echo "------------------------------------------------------------------"
    exit 2
fi

if eq0 "$w_rcpc" && eq0 "$w_sc" && eq0 "$w_dmb"; then
    echo " MCA SANITY PASS: plain WRC=$w_plain (harness is live) yet rcpc/sc/dmb all = 0."
    echo "   Every ORDERED mapping forbids the non-multi-copy-atomic outcome, so this"
    echo "   silicon behaves multi-copy-atomically — the property rcpc AND sc both rely on."
    echo "   This does NOT separately 'clear rcpc': rcpc == sc on WRC by construction. rcpc"
    echo "   is cleared as exact-TSO by the SB result in run_layerA.sh, not by this test."
else
    echo " ANOMALY: an ORDERED mapping let WRC through (rcpc=$w_rcpc, sc=$w_sc, dmb=$w_dmb)."
    echo "   All three should forbid it. This is either a harness artifact on this triple or"
    echo "   a genuine hardware MCA anomaly — NOT specific to rcpc (sc would fail the same"
    echo "   way). Re-run with a different CPUS triple; if it persists, do not run weak"
    echo "   translation on these cores and investigate the silicon."
fi
echo "------------------------------------------------------------------"
