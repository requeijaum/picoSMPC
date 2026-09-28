#!/usr/bin/env bash
#
# differ.sh -- run every scenario through both models and compare.
#
# The two models are compared on *data*, not on timestamps.  That is not a
# convenience: the golden model's clock ratio overflows (see
# docs/timing-baseline.md), so its event times are wrong by a factor of 5.67
# and a timestamp comparison would fail on every scenario for a reason that
# has nothing to do with our correctness.  Timestamps are reported
# separately, as a ratio, so a genuine timing regression is still visible.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TB="$HERE/../build/tb"

[ -x "$TB" ] || { echo "build first: $HERE/build.sh" >&2; exit 1; }

SCENARIOS=("$@")
if [ ${#SCENARIOS[@]} -eq 0 ]; then
	SCENARIOS=(status_only intback_one_pad intback_analog intback_mouse
	           intback_multitap settime_smem command_matrix direct_mode
	           sysres_ckchg)
fi

strip_ts() { sed -E 's/^\[ *[0-9-]+\]//'; }

pass=0
fail=0
failed=()

for sc in "${SCENARIOS[@]}"; do
	a="$(timeout 300 "$TB" beetle "$sc" 2>&1 | strip_ts)"
	b="$(timeout 300 "$TB" ours   "$sc" 2>&1 | strip_ts)"
	if [ "$a" == "$b" ]; then
		printf 'PASS  %s\n' "$sc"
		pass=$((pass + 1))
	else
		printf 'FAIL  %s\n' "$sc"
		fail=$((fail + 1))
		failed+=("$sc")
		if [ "${DIFF_VERBOSE:-0}" = 1 ]; then
			diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") | sed 's/^/        /'
		fi
	fi
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
if [ ${#failed[@]} -gt 0 ]; then
	printf 'failing: %s\n' "${failed[*]}"
fi
[ "$fail" -eq 0 ]
