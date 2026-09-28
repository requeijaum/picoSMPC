#!/usr/bin/env bash
# Two-column comparison of an SMPC scenario run against two oracles.
#
#   HARDWARE  sim/traces/<sc>.expect, derived by sim/gen_expect.py from the
#             BlueRetro controller-port driver and the SMPC's own report
#             framing.  Authoritative: a mismatch here is a FAIL.
#   BEETLE    sim/traces/<sc>.beetle, a recorded run of the Beetle backend.
#             A reference, not an oracle: Beetle has known disagreements
#             with the hardware reference (mouse report length, multi-tap
#             count nybble, OREG pre-fill), so a mismatch here is INFO.
#
# SF is compared in neither column.  Bits 7..1 of SF are open-bus: what they
# read back depends on the last value the SH-2 put on the data bus, not on
# anything the SMPC does.  The flag in bit 0 is exercised functionally --
# every poll in sim/tb.c waits on it -- so it does not need a dump.
#
# usage: sim/differ.sh [--rerecord] [scenario ...]
#   DIFF_VERBOSE=1   show every differing line of the Beetle column

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
TB="$ROOT/build/tb"
TRACES="$HERE/traces"

RERECORD=0
if [ "${1:-}" = "--rerecord" ]; then
	RERECORD=1
	shift
fi

[ -x "$TB" ] || { echo "differ: $TB missing -- run sim/build.sh" >&2; exit 2; }

if [ $# -gt 0 ]; then
	SCENARIOS=("$@")
else
	SCENARIOS=()
	while IFS= read -r f; do
		SCENARIOS+=("$(basename "$f" .expect)")
	done < <(ls "$TRACES"/*.expect 2>/dev/null)
	if [ ${#SCENARIOS[@]} -eq 0 ]; then
		echo "differ: no expectations -- run sim/gen_expect.py" >&2
		exit 2
	fi
fi

for sc in "${SCENARIOS[@]}"; do
	[ -f "$TRACES/$sc.expect" ] || {
		echo "differ: $TRACES/$sc.expect missing -- run sim/gen_expect.py" >&2
		exit 2
	}
	if [ ! -f "$TRACES/$sc.beetle" ] || [ "$RERECORD" = 1 ]; then
		"$TB" --trace beetle "$sc" > "$TRACES/$sc.beetle" || {
			echo "differ: beetle backend failed on $sc" >&2
			exit 3
		}
	fi
done

# --------------------------------------------------------------------------
# Hardware column.
#
# Reads the .expect file first (file 1) and our canonical trace second
# (file 2), then walks the two register dumps together.  Every derived byte
# is compared; every `--` byte is skipped.  Emits one line per problem and
# exits non-zero if there were any.
#
# The two formats are distinguished by their first field: the expect file
# starts its lines with OREG / SR / ---, the trace with REG.  `---` carries
# one field and so never reaches the byte loop.
# --------------------------------------------------------------------------
hardware_check() {
	local sc=$1
	awk -v sc="$sc" '
		FNR == NR {
			if ($1 == "OREG") {
				g++
				ng[g] = 0
				for (i = 2; i <= NF; i++) ev[g, ++ng[g]] = $i
				next
			}
			if ($1 == "SR") esr[g] = $2
			next
		}
		$1 == "REG" && $2 == "OREG" {
			t++; nt[t] = 0
			for (i = 3; i <= NF; i++) tv[t, ++nt[t]] = $i
			tmode = 1
			next
		}
		$1 == "REG" && $2 == "SR" { tsr[t] = $3; tmode = 0; next }
		END {
			bad = 0
			if (t != g) {
				printf "group count: want %d got %d\n", g, t
				bad++
			}
			for (k = 1; k <= g && k <= t; k++) {
				if (ng[k] != nt[k]) {
					printf "group %d: OREG length want %d got %d\n",
					       k, ng[k], nt[k]
					bad++
				}
				if (esr[k] != tsr[k]) {
					printf "group %d: SR want %s got %s\n", k, esr[k], tsr[k]
					bad++
				}
				n = ng[k] < nt[k] ? ng[k] : nt[k]
				for (i = 1; i <= n; i++) {
					w = ev[k, i]
					if (w == "--") continue
					got = tv[k, i]
					if (w == got) continue
					printf "group %d: OREG[%02X] want %s got %s\n",
					       k, i - 1, w, got
					bad++
				}
			}
			exit bad ? 1 : 0
		}
	' "$TRACES/$sc.expect" "$2"
}

# --------------------------------------------------------------------------
# Beetle column: informational.  Counts the lines of the canonical trace
# that differ, since the two runs already share a format (no timestamps).
# --------------------------------------------------------------------------
beetle_check() {
	local sc=$1 trace=$2
	local diffs
	# `diff` prints the mismatched pair for every hunk; only the count and,
	# under DIFF_VERBOSE, the first few lines are interesting.
	# SF is filtered out here as well as from the hardware column: its
	# bits 7..1 are open-bus and simply record the last byte the host put
	# down, so a host that never had to send a continue -- because the
	# report finished inside the COMREG write -- reads a different value
	# back without either implementation being wrong about the flag.
	diffs=$(diff "$trace" "$TRACES/$sc.beetle" 2>/dev/null |
	        grep '^[<>]' | grep -vE '^[<>] REG  SF  ' | wc -l || :)
	echo "$diffs"
}

HARDWARE_FAIL=0
BEETLE_DIFF=0
TOTAL=0
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

printf '%-18s  %-6s %-11s\n' "scenario" "HARDWARE" "BEETLE"
printf '%-18s  %-6s %-11s\n' "------------------" "------" "-----------"

for sc in "${SCENARIOS[@]}"; do
	TOTAL=$((TOTAL + 1))

	if ! "$TB" --trace ours "$sc" > "$TMP/ours.trace"; then
		printf '%-18s  %-6s %-11s\n' "$sc" "FAIL" "n/a"
		printf '      backend exited non-zero\n'
		HARDWARE_FAIL=$((HARDWARE_FAIL + 1))
		continue
	fi

	hw="PASS"
	if ! hardware_check "$sc" "$TMP/ours.trace" > "$TMP/hw.txt"; then
		hw="FAIL"
		HARDWARE_FAIL=$((HARDWARE_FAIL + 1))
	fi

	bt="PASS"
	diffs=$(beetle_check "$sc" "$TMP/ours.trace")
	if [ "$diffs" -gt 0 ]; then
		bt="INFO($diffs)"
		BEETLE_DIFF=$((BEETLE_DIFF + 1))
	fi

	printf '%-18s  %-6s %-11s\n' "$sc" "$hw" "$bt"
	if [ "$hw" = "FAIL" ]; then
		sed 's/^/      /' "$TMP/hw.txt"
	fi
	if [ "${DIFF_VERBOSE:-0}" = 1 ] && [ "$diffs" -gt 0 ]; then
		diff "$TMP/ours.trace" "$TRACES/$sc.beetle" 2>/dev/null |
			grep '^[<>]' | sed -n '1,40{s/^/    /;p}'
	fi
done

printf '\n'
if [ "$HARDWARE_FAIL" -eq 0 ]; then
	echo "hardware column: PASS ($TOTAL/$TOTAL)"
else
	echo "hardware column: $HARDWARE_FAIL of $TOTAL failing"
fi
echo "beetle column:   $BEETLE_DIFF of $TOTAL diverging (informational)"

[ "$HARDWARE_FAIL" -eq 0 ]
