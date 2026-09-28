# smpc-rp2350

A behavioural reimplementation of the Sega Saturn's SMPC — the Hitachi
HD404920FS / Sega 315-5744 — as portable C, on its way to running on an
RP2350B as a drop-in replacement for the original chip.

The original chip has no ESD protection on the controller port and burns out.
A modern replacement also fixes the Saturn's well-known battery drain, and can
accept a USB HID controller in place of a physical one.

## Status

| Phase | State |
|---|---|
| 0 — references, timing baseline | done |
| 1 — portable core skeleton + golden-model harness | done |
| 2 — command engine, INTBACK, RTC/SMEM | **mostly done**; the peripheral-report path is still being finished |
| 3 — pad protocol, multi-tap, virtual device | multi-tap and the self-clocking path work; the first-generation digital-pad path and the mouse do not yet match the reference |
| 4 — RP2350B firmware (PIO, timebase) | not started |
| 5 — RTC/NVRAM/STE on hardware | not started |
| 6 — hardware | not started |

`sim/differ.sh` reports the current state. It compares our core against
Mednafen's SMPC, which is compiled unmodified as a library — see
`reference/beetle/README.md`.

## Build and test

```sh
./sim/build.sh          # needs BEETLE_ROOT, see below
./sim/differ.sh         # all scenarios
./sim/differ.sh -v      # with diffs
./build/tb ours         # our core only, all scenarios
./build/tb beetle       # the golden model only
```

`BEETLE_ROOT` must point at a `beetle-saturn-libretro` checkout; it defaults
to `~/projects/saturn_emulator_for_chinese_handhelds/beetle-saturn-libretro`.
`arm-none-eabi-gcc`, `cmake` and `ninja` are already installed. `pico-sdk` and
`picotool` are not yet, and are not needed until phase 4.

## Layout

```
core/     the model.  Portable C99, no dependencies, no allocation after init.
sim/      testbench and the golden-model harness.
docs/     timing baseline and architecture notes.
reference/  golden model and reverse-engineering material.  Not built.
rtl/      RP2350B firmware.  Not written yet.
hw/       board notes.  Not started.
```

## Reading order

1. `docs/architecture.md` — what the pieces are, and the two or three things
   that are genuinely counter-intuitive.
2. `docs/timing-baseline.md` — every timing constant, where it came from, and
   the one that nobody knows.
3. `core/include/smpc/smpc.h` — the register map and the environment seam.
4. `core/src/smpc.c` — the command engine and the report sequencer.
5. `sim/tb.c` — the scenarios, which are the actual specification.

## Three findings worth knowing about up front

- **The SMPC's address pins are the SH-2's A2..A7, not A1..A6.** The register
  index is `(byte_address & 0x7F) >> 1`. Assuming otherwise shifts every
  register by one and produces a model that reads plausible garbage.

- **The reference implementation's clock ratio overflows.** Mednafen stores a
  32.32 fixed-point value in a `uint32_t`, which wraps, making its SMPC run at
  2.27 MHz instead of 4 MHz — everything 5.67× too slow. It cannot be used to
  check timing, which is why the differ compares data and effect order only.
  Arithmetic in `docs/timing-baseline.md`.

- **INTBACK is a 4-bit-per-transfer DMA spanning a vblank, not a byte dump.**
  Every HLE gets this wrong. It is why the peripheral half of the report is
  built as an interpreted instruction stream rather than straight-line code.

## Licence and provenance

The model is our own work. The golden model in `reference/beetle/` is Mednafen's,
GPLv3, unmodified and unlinked into anything we ship. The reverse-engineering
material in `reference/smpc-emulator/` is abrasive's, CC-BY. Sega's SMPC ROM
and the Saturn Service Manual are Sega's and are not redistributed here.
