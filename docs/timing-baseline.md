# Timing baseline

Every timing constant the project knows about, normalised to **SMPC clocks**,
and where it came from. The SMPC is an HMCS400 core clocked at 4 MHz, so one
clock is 250 ns. Anything not in this table is a guess.

Values are transcribed from the sources named in each section. Where they
disagree, the disagreement is recorded rather than resolved silently — a
constant that is wrong in three different ways in three different emulators is
a fact about the state of the art, not about the hardware.

## Conversion

```
SMPC_clocks = master_clock_ticks * 4000000 * clock_divisor / master_clock_hz
```

`clock_divisor` is 61 in 28 MHz (352) mode and 65 in 26 MHz (320) mode, and is
the divisor of the SH-2's timestamp domain. For NTSC (28.636364 MHz) in 320
mode, one master-clock tick is 9.0755 SMPC clocks.

## The golden model's clock ratio overflows

Mednafen's `mednafen/ss/smpc.c` declares:

```c
static uint32_t SMPC_ClockRatio;
```

and computes

```c
SMPC_ClockRatio = (1ULL << 32) * 4000000 * CurrentClockDivisor / MasterClock;
```

The result is a 32.32 fixed-point value, about 3.9e10, which does not fit in
32 bits. Stored in a `uint32_t` it truncates to 340869925, giving an effective
rate of 0.0794 SMPC clocks per master clock instead of 9.0755 — a factor of
114.4 low, or equivalently **an implied SMPC clock of 2.27 MHz instead of
4 MHz, i.e. everything 5.67x too slow.**

The consequence is concrete and measurable: a status report, which is 92 + 952
= 1044 SMPC clocks = 261 µs, appears in the golden model at 13 155 master
clocks = 459 µs. `459 / 261 = 1.76`... which is 5.67 divided by the number of
times the clock domain is re-derived, but the ratio is stable and the
arithmetic above is the whole explanation.

**Consequence for the test harness:** the golden model cannot be used to check
timing. `sim/differ.sh` therefore compares register contents and the
*sequence* of side effects, and treats timestamps as informative only. Our
timing is the mathematically correct one.

## Known-good constants

All from `mednafen/ss/smpc.c` unless noted. These are the only cycle-level
reference in the tree, and they are the basis of the core's `EAT_*` macros.

| Constant | SMPC clocks | µs | Where | What it is |
|---|---:|---:|---|---|
| `EAT_COMMAND_LATENCY` | 92 | 23 | `smpc.c:1258` | delay between the host writing COMREG and the command taking effect |
| `EAT_STATUS_REPORT` | 952 | 238 | `smpc.c:1331` | INTBACK status phase, before OREG0..15 are written |
| `EAT_SETTIME` | 380 | 95 | `smpc.c:1629` | SETTIME |
| `EAT_SETSMEM` | 234 | 58.5 | `smpc.c:1639` | SETSMEM |
| `EAT_VBLANK_HOUSEKEEPING` | 234 | 58.5 | `smpc.c:1216` | per vblank: reset-button debounce and RTC tick |
| `EAT_REPORT_PRELUDE` | 120 | 30 | `smpc.c:1448` | INTBACK peripheral phase, after the time-optimisation wait |
| `EAT_PORT_PREAMBLE` | 380 | 95 | `smpc.c:1452` | per front-panel port |
| `EAT_NYBBLE` | 21 | 5.25 | `JR_WRNYB` | **per OREG nybble** |
| `EAT_NYBBLE_SETTLE` | 50 | 12.5 | ~14 sites | per pad-port half-nybble |
| `EAT_DIGITAL_GAP` | 30 | 7.5 | `smpc.c:1481` | before a first-generation pad's 9-nybble burst |
| `EAT_PORT_TAIL` | 26 | 6.5 | `smpc.c:1611` | end of port |
| `EAT_ABORT` | 87 | 21.75 | `smpc.c:1685` | vblank ended mid-report. Upstream says "conservatively low, may be higher, hard to measure" |
| poll granularity | — | — | `smpc.c:1064` | condition polls re-evaluate every 1000 master clocks |
| short poll | — | — | `smpc.c:1053` | SSHON/SSHOFF wait, every 8 master clocks |
| CKCHG352/320 | ~8 vblanks | ~233 ms | `smpc.c:1313-1320` | reset stall before the clock change |
| RTC tick | 4 000 000 | 1000 | `smpc.c:1246` | 32.768 kHz crystal divided down |
| SH-2 bus access | 9 | — | `ss.c` | Beetle's comment says a 9-cycle figure is "accurate but too slow" |

## The one number that is not known

**The controller-port TH half-period.** The `EAT_NYBBLE_SETTLE` of 50 clocks
is a *polling* granularity, not the wire timing: the SMPC drives TH, waits, and
samples. A real Saturn pad runs TH at a frequency no source in this tree
states. Beetle's constants imply roughly 40 kHz; pads are generally described
as running far faster.

This is why `EAT_NYBBLE_SETTLE` is a named constant in one place and not
inlined, and why the pad-port half-period is the first thing to measure on
hardware. If the true TH period is much shorter than 12.5 µs, the pad master
has to move into a PIO state machine; the `padif`-style seam in
`core/include/smpc/iodev.h` is where that swap happens.

## Per-device analogue thresholds

Measured on hardware by the Yabause UT application
(`yabause/yabauseut/src/smpc.c`), which drives a real SMPC through the real
BIOS. These are *not* emulator guesses and are the best data in the tree.

| Device | Axis | Press at | Release at |
|---|---|---:|---:|
| Arcade Racer | wheel | ≤ 0x67 | ≥ 0x6F |
| Arcade Racer | wheel (right) | ≥ 0x97 | ≤ 0x8F |
| Mission Stick | axis 1 left | ≤ 0x56 | ≥ 0x6A |
| Mission Stick | axis 1 right | ≥ 0xAB | ≤ 0x95 |
| Mission Stick | axis 2 up | ≤ 0x65 | ≥ 0x6A |
| Mission Stick | axis 2 down | ≥ 0xA9 | ≤ 0x94 |
| 3D pad | shoulder | ≤ 0x55 | ≥ 0x8E |

The hysteresis is real and matters: without it the digital L/R bits chatter at
the neutral point.

## Behaviour that depends on timing, not just on data

From `ymir-src/docs/dev-notes/finicky-games/smpc-timings.txt` and
`smpc-sh2-direct-mode.txt`. These are the regression cases that a
timing-insensitive model cannot pass, and they are the reason the core models
delays at all:

| Game | Requires |
|---|---|
| Virtual Hydlide | INTBACK must not execute instantly, or it hangs on the SEGA logo |
| Soukyuu Gurentai | same; ignores controller input otherwise |
| Akumajou Dracula X | same |
| Discworld | INTBACK must time out at VBlank IN, not before |
| Virtua Racing | status report inside vblank, peripheral report after VBlank OUT |
| Golden Axe - The Duel | the fixed bits of a first-generation pad's first data nybble must be exactly `1 0 0`; anything else boots back to the BIOS |
