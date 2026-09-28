#!/usr/bin/env bash
#
# Populate reference/ with the third-party trees the test harness needs.
#
# These are deliberately NOT in git.  Two of them contain material that does
# not belong in a public repository -- Sega's SMPC ROM and pages of the Saturn
# Service Manual -- and one of them (BlueRetro) is 3 MB of somebody else's
# history.  Cloning them on demand keeps this repository to its own ~1 MB of
# work while leaving the citations in sim/gen_expect.py verifiable, because
# verify_citations() re-reads the very lines it cites and refuses to run if
# they have moved.
#
# Usage:
#   tools/fetch-reference.sh          # clone what is missing
#   tools/fetch-reference.sh --check  # report what is missing, clone nothing
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REF="$ROOT/reference"
CHECK=0

if [ "${1:-}" = "--check" ]; then
	CHECK=1
fi

BLUERETRO_URL="https://github.com/darthcloud/BlueRetro"
BLUERETRO_REF="master"
SMPCTOOL_URL="https://codeberg.org/abrasive/smpc-emulator"

BEETLE_ROOT="${BEETLE_ROOT:-$HOME/projects/saturn_emulator_for_chinese_handhelds/beetle-saturn-libretro}"

missing=0

# --- Beetle's SMPC: the golden model, vendored as a 3-file extract ----------
# reference/beetle/ IS tracked.  This check only confirms the real checkout
# the harness actually compiles against is present.
if [ -f "$BEETLE_ROOT/mednafen/ss/smpc.c" ]; then
	echo "ok    beetle-saturn-libretro (BEETLE_ROOT) -- golden model"
else
	echo "MISS  beetle-saturn-libretro -- set BEETLE_ROOT to a checkout" >&2
	missing=$((missing + 1))
fi

# --- BlueRetro: the peripheral-side reference -------------------------------
if [ -f "$REF/bluRetro/main/wired/sega_io.c" ]; then
	echo "ok    reference/bluRetro -- controller-port reference"
elif [ "$CHECK" = 1 ]; then
	echo "MISS  reference/bluRetro -- $BLUERETRO_URL" >&2
	missing=$((missing + 1))
else
	echo "==> cloning BlueRetro (--depth 1, ~3 MB)"
	git clone --depth 1 --branch "$BLUERETRO_REF" "$BLUERETRO_URL" \
		"$REF/bluRetro"
fi

# --- abrasive's HMCS400 project: the only non-emulator source in the tree ----
if [ -f "$REF/smpc-emulator/HARDWARE.md" ]; then
	echo "ok    reference/smpc-emulator -- HMCS400 hardware notes"
elif [ "$CHECK" = 1 ]; then
	echo "MISS  reference/smpc-emulator -- $SMPCTOOL_URL" >&2
	missing=$((missing + 1))
else
	echo "==> cloning smpc-emulator (this one is ~65 MB)"
	git clone --depth 1 "$SMPCTOOL_URL" "$REF/smpc-emulator"
	echo "    reference/smpc-emulator/dump/ holds Sega's SMPC ROM.  It is not"
	echo "    needed for an HLE model and must not be redistributed."
fi

if [ "$missing" != 0 ]; then
	echo >&2
	echo "$missing reference tree(s) missing; see reference/README.md." >&2
	exit 1
fi

echo
echo "all reference trees present.  next: sim/build.sh && sim/differ.sh"
