#!/bin/sh
# run_mca.sh — the MULTI-COPY-ATOMICITY gate for rcpc (Layer A, part 2).
#
# The 2-thread run_layerA.sh proves the W->R half of TSO-exactness. It CANNOT
# see multi-copy atomicity. This 3-thread WRC test does: it answers the one open
# question about the cheap mapping — does LDAPR/STLR (rcpc) keep the non-MCA
# outcome FORBIDDEN, as x86-TSO requires?
#
#   WRC:  T0: x=1     T1: r1=x; y=1     T2: r2=y; r3=x
#   witness = r1==1 && r2==1 && r3==0   (T1 saw x and published y; T2 saw y but
#   NOT x — a non-multi-copy-atomic observation). FORBIDDEN under x86-TSO.
#
# Per mapping, lowering the shared accesses with the translator's instructions:
#   plain : LDR/STR                    -> witness CAN fire on weak ARM (SENSITIVITY
#                                         CONTROL: proves the harness sees non-MCA)
#   rcpc  : LDAPR/STLR                 -> witness must be 0 IFF LDAPR preserves MCA
#   sc    : LDAR/STLR                  -> 0 (RCsc is MCA-safe)
#   dmb   : LDR;DMB ISHLD / DMB ISHST;STR -> 0 (DMB is MCA-safe)
#
# DECISION:
#   - plain fires (>0) AND rcpc==0  => LDAPR/STLR preserves MCA on this silicon.
#     Combined with run_layerA.sh's W->R result, rcpc is exact-TSO: CLEARED.
#   - plain fires AND rcpc>0 (while sc==dmb==0) => LDAPR/STLR BREAKS MCA here:
#     rcpc is NOT exact-TSO. Ship sc/dmb. (This is the outcome to watch for.)
#   - plain==0 => INCONCLUSIVE: this lockstep harness didn't expose non-MCA on
#     this triple (WRC yield is low). Try other cores / more ITERS, or use
#     litmus7 (engineered for high WRC yield) for the definitive battery.
#
# Env: ITERS (default 20000000), CPUS="a b c" (force a core triple),
#      BIN (default ./oryx_litmus).  POSIX sh; runs in Termux / the Android shell.
set -eu

ITERS="${ITERS:-20000000}"
BIN="${BIN:-./oryx_litmus}"
[ -x "$BIN" ] || { echo "building $BIN ..."; make >/dev/null; }

wrc() {   # args: map a b c [iters]
    "$BIN" --test wrc --map "$1" --iters "${6:-$ITERS}" \
        --cpu-a "$2" --cpu-b "$3" --cpu-c "$4" --json \
        | sed -n 's/.*"witnesses":\([0-9]*\).*/\1/p'
}

# candidate core triples: a spread across clusters; the one with the highest
# plain-WRC yield (most sensitive) wins. WRC needs three distinct cores.
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
    w=$(wrc plain "$ta" "$tb" "$tc" "$SCAN_ITERS"); [ -z "$w" ] && w=0
    printf "   triple %-8s plain WRC(%s iters) = %s\n" "$ta,$tb,$tc" "$SCAN_ITERS" "$w"
    if [ "$w" -gt "$best" ] 2>/dev/null; then best=$w; A=$ta; B=$tb; C=$tc; fi
done
IFS=$OIFS
[ -n "$A" ] || { A=0; B=1; C=2; }
echo "   -> using triple $A,$B,$C (plain WRC = $best)"

echo "=================================================================="
echo " Project Oryx — MCA gate: does LDAPR/STLR keep WRC forbidden?"
echo " cores = $A,$B,$C   iters/measurement = $ITERS"
echo "=================================================================="
echo ""
printf "%-7s %-16s %s\n" "map"   "WRC(forbidden)" "reading"
printf "%-7s %-16s %s\n" "-----" "--------------" "-------"

w_plain=$(wrc plain "$A" "$B" "$C"); w_rcpc=$(wrc rcpc "$A" "$B" "$C")
w_sc=$(wrc sc "$A" "$B" "$C");       w_dmb=$(wrc dmb "$A" "$B" "$C")
for x in "plain:$w_plain:sensitivity control (should fire on weak ARM)" \
         "rcpc:$w_rcpc:MCA question — must be 0 to clear rcpc" \
         "sc:$w_sc:MCA-safe (expect 0)" \
         "dmb:$w_dmb:MCA-safe (expect 0)"; do
    m=$(echo "$x" | cut -d: -f1); v=$(echo "$x" | cut -d: -f2); r=$(echo "$x" | cut -d: -f3-)
    printf "%-7s %-16s %s\n" "$m" "$v" "$r"
done

echo ""
echo "------------------------------------------------------------------"
gt0() { [ "$1" -gt 0 ] 2>/dev/null; }
eq0() { [ "$1" = "0" ]; }

if ! gt0 "$w_plain"; then
    echo " INCONCLUSIVE: plain WRC never fired ($w_plain). This lockstep harness"
    echo "   did not expose the non-MCA outcome on cores $A,$B,$C (WRC yield is low)."
    echo "   A 0 for rcpc here proves NOTHING — the harness couldn't see the effect."
    echo "   Try: CPUS=\"0 4 7\" sh run_mca.sh  or  ITERS=500000000 sh run_mca.sh,"
    echo "   or use litmus7 (built for high WRC yield) for the definitive battery."
    echo "------------------------------------------------------------------"
    exit 2
fi

# plain fired -> the harness CAN see non-MCA on this triple. Now judge the mappings.
if ! eq0 "$w_sc" || ! eq0 "$w_dmb"; then
    echo " HARNESS SUSPECT: plain fired ($w_plain) but a KNOWN-MCA-safe mapping also"
    echo "   fired (sc=$w_sc, dmb=$w_dmb). sc/dmb must forbid WRC — a nonzero here means"
    echo "   a harness artifact (drift/aliasing), not a real result. Do not trust the"
    echo "   rcpc verdict on this triple; re-run with a different CPUS triple."
    echo "------------------------------------------------------------------"
    exit 3
fi

if eq0 "$w_rcpc"; then
    echo " MCA PRESERVED: plain WRC=$w_plain (harness is sensitive) yet rcpc WRC=0,"
    echo "   matching sc=0 and dmb=0. LDAPR/STLR keeps the non-MCA outcome FORBIDDEN"
    echo "   on this silicon. Together with run_layerA.sh's W->R exactness, this"
    echo "   CLEARS rcpc as exact x86-TSO for the tested litmus family."
    echo "   (Strengthen further with IRIW and higher ITERS before production.)"
else
    echo " *** MCA VIOLATED — rcpc WRC=$w_rcpc (plain=$w_plain, sc=0, dmb=0). ***"
    echo "   LDAPR/STLR let a non-multi-copy-atomic outcome through that x86-TSO"
    echo "   forbids. rcpc is NOT exact-TSO on this hardware. Ship sc (LDAR/STLR) or"
    echo "   dmb, which held. This is exactly the failure the MCA gate exists to catch."
fi
echo "------------------------------------------------------------------"
