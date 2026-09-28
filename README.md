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
| 2 — command engine, INTBACK, RTC/SMEM | command engine, status report, SETTIME, SETSMEM, area code, reset debounce all match; the peripheral-report path is unfinished |
| 3 — pad protocol, multi-tap, virtual device | the self-clocking device path matches; the first-generation digital-pad path, the mouse and multi-tap do not yet |
| 4 — RP2350B firmware (PIO, timebase) | not started |
| 5 — RTC/NVRAM/STE on hardware | not started |
| 6 — hardware | not started |

`sim/differ.sh` reports the current state. It compares our core against
Mednafen's SMPC, which is compiled unmodified as a library — see
`reference/beetle/README.md`.

**2 of 9 scenarios pass.**  The peripheral-report path now runs to completion
and pulses its interrupts correctly, but the bytes it writes are still wrong.
Four real defects were fixed along the way — all found by reading other
implementations, not by the harness: The failures are not all the same kind, and it is
worth being precise about which is which rather than reading the count:

| Scenario | Divergence | Assessment |
|---|---|---|
| `settime_smem` | one bit of the SF open-bus read (`0x80` vs `0x10`) | every register byte, SR and every side effect match; the difference is which value last sat on the bus, so it is most likely a harness artifact of the INTBACK continue handshake, not a model difference — unconfirmed |
| `intback_one_pad`, `intback_analog` | the report is cut short after two nybbles | real gap in the report engine |
| `intback_mouse` | ID1 decode: 0x03 instead of 0xE3 | real gap in the mouse's id-nybble de-scrambling |
| `intback_multitap` | the adapter header is read as a device id, then the sub-slot loop stalls | real gap in the multi-tap path |
| `direct_mode` | the final INTBACK produces no report | real gap, same as the above |
| `sysres_ckchg` | the scenario itself is broken (it drives the virtual clock backwards); both models fail it the same way, which is a harness bug, not a model one |

- a use-after-free: a port cached a device pointer that `smpc_set_peripheral`
  then freed
- an out-of-bounds index: `cur_port` was incremented without wrapping and
  indexes `s->port[]` directly
- three multi-tap guards with their sense inverted, so a directly-connected
  pad's id and size were overwritten with `0xFF` and its data length became 15
- the front-panel port loop had no trip count, so it ran exactly once

So: the command engine, the status report and the report sequencer's control
flow are solid; the bytes the sequencer produces in the self-clocking block
are not yet right. That is debugging inside a design that is in place, not
missing design.

The remaining divergence is narrow and reproducible: dump the OREG nybble
writes with `./build/tb` under a debug build and compare against the expected
`F1 02 FF FF F0`. The `id_tap` pair should be `0xF1` and is coming out as
`0x05`, which means `OP_SET_IDTAP_DIRECT` is not taking effect where the
program expects it. Everything before and after that point in the exchange is
byte-correct.

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
3. `docs/controller-port.md` — the controller port as answered by
   BlueRetro, which drives real hardware and contradicts the emulators.
4. `docs/smpc-implementations.md` — how the four SMPC models in this tree
   disagree, and which side this project takes and why.
5. `core/include/smpc/smpc.h` — the register map and the environment seam.
6. `core/src/smpc.c` — the command engine and the report sequencer.
7. `sim/tb.c` — the scenarios, which are the actual specification.

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

- **There is no SC or RMD line on the controller port.** Every emulator models
  one. The Saturn-versus-Mega-Drive distinction is carried in the terminator
  nybble of each transfer, and the four data lines are physically named
  `R`, `L`, `D`, `U` — after the directions they drive — not `D0`-`D3`.

## Licence and provenance

The model is our own work. The golden model in `reference/beetle/` is Mednafen's,
GPLv3, unmodified and unlinked into anything we ship. The reverse-engineering
material in `reference/smpc-emulator/` is abrasive's, CC-BY. Sega's SMPC ROM
and the Saturn Service Manual are Sega's and are not redistributed here.
