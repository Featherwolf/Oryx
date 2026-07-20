#!/bin/sh
# oryx-run — apply the community-best Oryx profile for a game, then launch it.
#
# This is Part C (auto-tuning) reaching a *running app*: it resolves the winning
# configuration for a game on this device, exports it as the environment the
# Box64/Wine/DXVK/Turnip stack reads, and then execs your launch command. No
# emulator changes — it just stops you hand-tuning flags per game.
#
# Usage:
#   oryx-run --profiles <dir> --game <build_hash> [--device SM8850] [--hw-tso 0|1] \
#            -- <launch command...>
#
# Example (Termux + Box64 + Wine running a Windows game):
#   oryx-run --profiles ./profiles --game cyberpunk2077_v210 -- \
#            box64 wine /path/Cyberpunk2077.exe
#
# The device's hardware-TSO capability defaults to 0 (the measured S26 Ultra
# reality — no kernel prctl). If Part A ever lands on your device, pass --hw-tso 1
# and the profile engine will select the hardware-TSO variant automatically.
#
# Requires the `oryxprofile` binary (build: make -C src/liboryxprofile). Point at
# it with ORYXPROFILE=/path/to/oryxprofile, else it's looked up on PATH / nearby.

set -eu

PROFILES=""
GAME=""
DEVICE="SM8850"
HW_TSO="0"
DRYRUN=0

# Locate oryxprofile: env override, PATH, or the build tree next to this script.
here=$(dirname "$0")
ORYXPROFILE="${ORYXPROFILE:-}"
if [ -z "$ORYXPROFILE" ]; then
    if command -v oryxprofile >/dev/null 2>&1; then
        ORYXPROFILE=oryxprofile
    elif [ -x "$here/../src/liboryxprofile/oryxprofile" ]; then
        ORYXPROFILE="$here/../src/liboryxprofile/oryxprofile"
    else
        echo "oryx-run: cannot find 'oryxprofile' (set ORYXPROFILE=/path or build it)" >&2
        exit 1
    fi
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --profiles) PROFILES="$2"; shift 2 ;;
        --game)     GAME="$2";     shift 2 ;;
        --device)   DEVICE="$2";   shift 2 ;;
        --hw-tso)   HW_TSO="$2";   shift 2 ;;
        --dry-run)  DRYRUN=1;      shift 1 ;;
        --)         shift;         break ;;
        *) echo "oryx-run: unknown option $1" >&2; exit 2 ;;
    esac
done

[ -n "$PROFILES" ] || { echo "oryx-run: --profiles required" >&2; exit 2; }
[ -n "$GAME" ]     || { echo "oryx-run: --game required" >&2; exit 2; }

# Resolve the winning profile into environment exports.
ENV=$("$ORYXPROFILE" env "$PROFILES" "$GAME" "$DEVICE" "$HW_TSO") || {
    echo "oryx-run: no profile for '$GAME' on $DEVICE (hw_tso=$HW_TSO) — launching untuned" >&2
    ENV=""
}

if [ -n "$ENV" ]; then
    echo "oryx-run: applying Oryx profile for '$GAME' on $DEVICE (hw_tso=$HW_TSO):" >&2
    echo "$ENV" | sed 's/^/    /' >&2
    eval "$ENV"
fi

if [ "$DRYRUN" = "1" ] || [ $# -eq 0 ]; then
    # Print the resulting environment and exit (useful for testing/inspection).
    echo "ORYX_BACKEND=${ORYX_BACKEND:-} ORYX_MM_TSO=${ORYX_MM_TSO:-} ORYX_TURNIP_BUILD=${ORYX_TURNIP_BUILD:-} ORYX_FSR=${ORYX_FSR:-}"
    [ $# -eq 0 ] && exit 0
fi

# Hand off to the real launch command with the tuned environment in place.
exec "$@"
