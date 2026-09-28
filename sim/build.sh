#!/usr/bin/env bash
#
# build.sh -- builds the testbench against the golden model (Beetle's SMPC)
# and against our own core, then diffs the two traces.
#
# BEETLE_ROOT must point at a beetle-saturn-libretro checkout containing
# mednafen/ss/smpc.c.  The golden objects are the *unmodified* upstream
# sources -- see reference/beetle/README.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$ROOT/build"
BEETLE_ROOT="${BEETLE_ROOT:-$HOME/projects/saturn_emulator_for_chinese_handhelds/beetle-saturn-libretro}"

if [ ! -f "$BEETLE_ROOT/mednafen/ss/smpc.c" ]; then
	echo "error: BEETLE_ROOT=$BEETLE_ROOT does not look like beetle-saturn-libretro" >&2
	echo "       (expected \$BEETLE_ROOT/mednafen/ss/smpc.c)" >&2
	exit 1
fi

CFLAGS=(-O2 -g -std=gnu11 -Wall -Wno-unused-function)
INC=(-I"$BEETLE_ROOT/mednafen" -I"$BEETLE_ROOT/mednafen/ss"
     -I"$BEETLE_ROOT/libretro-common/include" -I"$BEETLE_ROOT"
     -I"$BEETLE_ROOT/mednafen/video" -I"$HERE" -I"$ROOT/core/include"
     -I"$ROOT/core/src" -I"$ROOT/sim" -I"$ROOT/sim/golden")

mkdir -p "$BUILD"

# --- our own core (portable, no external deps) -------------------------
for f in "$ROOT"/core/src/*.c; do
	[ -e "$f" ] || continue
	o="$BUILD/$(basename "${f%.c}").o"
	gcc "${CFLAGS[@]}" -c "$f" -o "$o" "${INC[@]}"
done

# --- shared testbench --------------------------------------------------
gcc "${CFLAGS[@]}" -c "$HERE/tb.c"        -o "$BUILD/tb.o"        "${INC[@]}"
gcc "${CFLAGS[@]}" -c "$HERE/trace.c"     -o "$BUILD/trace.o"     "${INC[@]}"
gcc "${CFLAGS[@]}" -c "$HERE/tb_main.c"   -o "$BUILD/tb_main.o"   "${INC[@]}"
gcc "${CFLAGS[@]}" -c "$HERE/backend_beetle.c" -o "$BUILD/backend_beetle.o" "${INC[@]}"
gcc "${CFLAGS[@]}" -c "$HERE/backend_ours.c"   -o "$BUILD/backend_ours.o"   "${INC[@]}"

# --- golden: upstream Beetle, unmodified -------------------------------
gcc "${CFLAGS[@]}" -w -c "$ROOT/reference/beetle/smpc.c"          -o "$BUILD/be_smpc.o"          "${INC[@]}"
gcc "${CFLAGS[@]}" -w -c "$ROOT/reference/beetle/smpc_iodevice.c" -o "$BUILD/be_smpc_iodevice.o" "${INC[@]}"
gcc "${CFLAGS[@]}" -c "$HERE/golden/shim.c" -o "$BUILD/shim.o" "${INC[@]}"

# --- link ----------------------------------------------------------------
# Both binaries carry both backends and pick one at runtime, so either can be
# run against the same scenario list and their traces diffed.
CORE_OBJS=()
for f in "$ROOT"/core/src/*.c; do
	[ -e "$f" ] || continue
	CORE_OBJS+=("$BUILD/$(basename "${f%.c}").o")
done

COMMON=("$BUILD/tb_main.o" "$BUILD/tb.o" "$BUILD/trace.o"
        "$BUILD/backend_beetle.o" "$BUILD/backend_ours.o"
        "$BUILD/be_smpc.o" "$BUILD/be_smpc_iodevice.o" "$BUILD/shim.o"
        "${CORE_OBJS[@]}")

gcc -o "$BUILD/tb" "${COMMON[@]}"
echo "built $BUILD/tb  -- run as: ./tb beetle | ./tb ours"
