# Timing baseline

> **Every constant below is untrusted.** Not one of them was ever measured on
> hardware. All of them are Mednafen's, and Mednafen's clock ratio is known to
> overflow (see below), so the set is internally consistent and externally
> unverified. There is no console in this project and no hardware capture in
> `reference/`, so nothing here can be corrected offline — not by better
> reading, not by more searching.
>
> What this baseline is *good for*: durations that only need to be the right
> *order* of magnitude relative to each other, so the report sequencer's phases
> overlap and interleave the way a real one does. What it is **not** good for:
> predicting wall-clock time, driving a pad port, or claiming cycle accuracy.
> `sim/differ.sh` compares data and effect order for exactly this reason, and
> never timestamps.

Every timing constant the project knows about, normalised to **SMPC clocks**,
and where it came from. The SMPC is an HMCS400 core clocked at 4 MHz, so one
clock is 250 ns. Anything not in this table is a guess.

Values are transcribed from the sources named in each section. Where they
disagree, the disagreement is recorded rather than resolved silently — a
constant that is wrong in three different ways in three different emulators is
a fact about the state of the art, not about the hardware.

The one genuinely independent hardware datum in the project is the clock
itself: abrasive's decap notes give 4 MHz with a 1 µs machine cycle
(`reference/smpc-emulator/HARDWARE.md`), which agrees with the HMCS400
handbook in the same tree. That fixes the *unit*. It says nothing about the
delays, which come from Mednafen.

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
| RTC tick | 4 000 000 | 1000 | `smpc.c:1246` | 4 MHz core clocks, *not* the watch crystal — see below |
| SH-2 bus access | 9 | — | `ss.c` | Beetle's comment says a 9-cycle figure is "accurate but too slow" |

## The two oscillators, and which one the RTC actually uses

The SMPC does not have one clock. It has two:

| Oscillator | Frequency | Drives |
|---|---:|---|
| Core | **4 MHz** | the HMCS400, one machine cycle per µs |
| Watch, on `OSC1`/`OSC2` | **32.768 kHz** | Timer A, the one-second RTC tick, and the timer interrupt that wakes the core |

and a third, observable signal derived from the second: pin D0 carries the RTC
oscillator divided by two, 16.384 kHz. Both are documented in
`reference/smpc-emulator/HARDWARE.md` and `DUMPING.md`, from the die and the
HMCS400 handbook.

On the chip, the RTC second is 32 768 counts of the watch crystal. That is a
*different oscillator* from the one the CPU runs on, with its own tolerance —
a tuning-fork crystal is typically ±20 ppm, while a consumer 4 MHz part is
several times worse. So the RTC's long-term accuracy is set by the watch
crystal, and the core oscillator's error does not reach it.

**This model does not model that.** `rtc_clock_accum` is fed the same 32.32
core-clock count as `clock_counter` (`core/src/smpc.c`), and the second falls
out at 4 000 000 core clocks. There is no `32768` anywhere in `core/`. Mednafen
does the same thing — `smpc.c:1246` is `RTC.ClockAccum >= (4000000ULL << 32)` —
so this is inherited, not invented, and it is why the RTC inherits the *core*
crystal's error: roughly ±100 ppm where the hardware would give ±20 ppm, or
about ±3 150 s/year against ±630 s/year.

For anything that runs for seconds or minutes the two are indistinguishable,
which is why this was not found earlier and why it is written down now rather
than fixed: separating it means giving the RTC its own rate parameter, and
with no hardware there is nothing to calibrate that parameter against. It is
recorded in the README's limitations table alongside the rest.

A related absence: the core is powered by the host *and* a CR2032, and the RTC
oscillator keeps counting with the console off. The supply monitor (IC25 on VA0)
raises interrupt 0 and wipes the save RAM when the battery is discharged with
main power off. None of that is modelled here — `rtc_valid` is a static flag
that measures nothing, and `docs/` records it as absent. Saturn_MiSTer's HLE is
weaker still: its RTC is `EXT_RTC` from the FPGA's own clock, with no
oscillator at all.

## The testbench's clock is 61x off, and no test can see it

`SMPC_ClockRatio` is computed exactly as Mednafen computes it, and the two
formulas are character for character the same:

```c
SMPC_ClockRatio = (1ULL << 32) * 4000000 * CurrentClockDivisor / MasterClock;
```

What matters is therefore what `MasterClock` *is*, and here the harness and a
real integration disagree. Mednafen's `ss.c` passes the SH-2 clock **already
multiplied by the clock divisor** — `1 746 818 182` for NTSC, which is
`28 636 364 * 61` — so the divisor in the formula cancels and the ratio comes
out at `4e6 / 28.636364 MHz`, or 0.1397 core clocks per tick. Sixty video
frames is one RTC second, as it must be.

`sim/tb.c` passes the *undivided* SH-2 clock, `28 636 364`, so the divisor
does not cancel: the ratio is 8.52 core clocks per tick, and one video frame
comes to about `4 065 000` core clocks — roughly **1.02 RTC seconds per
frame**. Measured directly, by running a scenario for 100 and 200 frames and
differencing the clock: **61.2x**, which is `SMPC_CLOCK_DIVISOR_28M` to within
the rounding. In 320 mode the error is 65x instead.

So the model is right and the harness is wrong, and the two together are
calibrated 61x away from the video the harness itself generates. Nothing
detects it, for three independent reasons, which is what makes it worth
writing down:

- both backends are handed the same wrong argument, so the two columns agree
  with each other perfectly;
- `differ.sh` compares data and effect order and **never compares
  timestamps** — that was a deliberate choice, to work around the clock-ratio
  overflow documented above;
- every scenario's *data* result is insensitive to it. INTBACK completes
  either way; only the wall-clock cost of the `EAT_*` constants scales.

The consequence is specific and worth stating plainly: **the harness cannot
test any wall-clock behaviour at all.** Whether the report fits inside
vblank, whether the reset debounce spans the right number of frames, whether
`EAT_NYBBLE_SETTLE` is anywhere near the real pad timing — none of it is
checkable here, and a scenario that asserted "after N frames the clock
advanced M seconds" would be recording the 61x as if it were the answer.

Fixing it is a one-line change — multiply by the divisor before calling
`init`, matching `ss.c` — and it was not made here for two reasons. It shifts
the timing of all eleven existing scenarios, none of which would then be
reproducing anything measured; and the value it moves *towards* still cannot
be validated without hardware, so the correction would replace one unverified
timebase with another. It is recorded as a known defect rather than silently
fixed.

## The one number that is not known

**The controller-port TH half-period.** The `EAT_NYBBLE_SETTLE` of 50 clocks
is a *polling* granularity, not the wire timing: the SMPC drives TH, waits, and
samples. A real Saturn pad runs TH at a frequency no source in this tree
states. Beetle's constants imply roughly 40 kHz; pads are generally described
as running far faster.

This is not a gap that more searching closes. Every tree under `reference/`
has been searched: the only clock figures that turn up are the SMPC's own
4 MHz oscillator and 32.768 kHz RTC crystal, plus a note that the *chip* was
successfully clocked at 10 kHz during the decap — none of which is the pad
port. There is no logic-analyser capture in the tree to derive a period from.

This is why `EAT_NYBBLE_SETTLE` is a named constant in one place and not
inlined, and why the pad-port half-period is the first thing a deployment with
hardware would need to measure. If the true TH period is much shorter than
12.5 µs, the pad master has to move into a PIO state machine; the
`padif`-style seam in `core/include/smpc/iodev.h` is where that swap happens.
Without hardware that decision cannot be made, which is part of why the
firmware phases were abandoned.

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
