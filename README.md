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
| 2 — command engine, INTBACK, RTC/SMEM | done — command engine, status report, SETTIME, SETSMEM, area code, reset debounce and the peripheral-report path all match |
| 3 — pad protocol, multi-tap, virtual device | done — digital pad, 3D pad in both modes, mouse, and the multi-tap all match the hardware-derived expectations |
| 4 — RP2350B firmware (PIO, timebase) | not started |
| 5 — RTC/NVRAM/STE on hardware | not started |
| 6 — hardware | not started |

`sim/differ.sh` reports the current state, in two columns:

- **HARDWARE** — `sim/traces/<sc>.expect`, derived by `sim/gen_expect.py`
  from the BlueRetro controller-port driver and the SMPC's own report
  framing. Authoritative: a mismatch fails the run.
- **BEETLE** — `sim/traces/<sc>.beetle`, a recorded run of Mednafen's SMPC
  compiled unmodified as a library (see `reference/beetle/README.md`).
  A reference, not an oracle: Beetle has known disagreements with the
  hardware reference, so a mismatch here is reported as `INFO` and never
  fails.

SF is compared in neither column. Bits 7..1 are open-bus — they read back
whatever the SH-2 last put on the data bus — and bit 0 is exercised
functionally by every poll in `sim/tb.c`, so a dump would only record which
of the two host interaction sequences happened to run.

**11 of 11 scenarios pass the hardware column; 10 of 11 match Beetle.**

| Scenario | State |
|---|---|
| `status_only`, `command_matrix`, `sysres_ckchg`, `settime_smem` | byte-identical on both columns |
| `intback_one_pad`, `intback_analog`, `intback_digital_buttons`, `intback_multitap`, `intback_multitap_p1`, `direct_mode` | byte-identical on both columns |
| `intback_mouse` | passes the hardware column; `INFO` against Beetle on the payload bytes |

The one Beetle divergence is the documented mouse payload-length
disagreement: BlueRetro sends four payload bytes for a standalone mouse
(`sega_io.c:308-321`) and only a mouse behind a multi-tap uses `0xE3`
(`sega_io.c:379`), while Beetle force-substitutes `0xE3` and reads three
bytes for both (`beetle/smpc.c:1525-1526`). `sim/gen_expect.py` marks those
bytes `--` rather than picking a side; the reasoning is in
`docs/controller-port.md`.

### Defects found so far

Read from other implementations, not guessed at. The list is cumulative:

- a use-after-free: a port cached a device pointer that `smpc_set_peripheral`
  then freed
- an out-of-bounds index: `cur_port` was incremented without wrapping and
  indexes `s->port[]` directly
- three multi-tap guards with their sense inverted, so a directly-connected
  pad's id and size were overwritten with `0xFF` and its data length became 15
- the front-panel port loop had no trip count, so it ran exactly once
- `OP_ENDREPEAT` popped its loop frame on the branch that jumps *back* and
  kept it on the branch that exits, so nested loops overwrote the outer
  loop's start address — the report ran port 0 twice and never reached port 1
- the multi-tap's count nybble was sampled with TL high instead of TL low,
  which is always zero, so the sub-slot loop ran once instead of once per pad
- `OP_SET_ID2` recomputed `is_tap`, so the sub-slot id read cleared the flag
  and from the second slot on the report fell a byte out of step with the
  adapter's stream
- `smpc_set_multitap` wired sub-slot *i* to `devices[port + 1 + i]`, skipping
  port 0's own pad; Beetle's `MapPorts` walks a cursor that includes it
- `ReadCount` took the low nybble of an all-ones `id2`, turning "no payload"
  into fifteen bytes and running the report three times over the 64-nybble
  DMA window
- the report's loops are do-whiles, so a zero bound still ran once; the two
  loops that can legitimately be empty (a tap with no pads, a sub-slot with
  no payload) needed an explicit guard in front of them
- `skip_empty_tap`'s guard was patched with `patch_if`, which overwrites the
  whole argument; `OP_IF_CTRMAX_ZERO` packs its counter into bits 0-3 and its
  target into bits 8-15, so the op read `ctr_max[15]` — out of bounds on a
  four-entry array — and, when that garbage happened to be zero, set `pc` to
  `arg >> 8`, which is 0, and looped the report from the start forever. It
  passed by luck: the out-of-bounds read had to come back non-zero, and only
  the port order decided that
- `smpc_iodev_update_input` had no `case SMPC_DEV_GAMEPAD`, so a
  first-generation pad's button state was never loaded and it reported every
  button released whatever the host set
- `digital_bus` put the button nybble on the wire without inverting it. The
  four data lines are active low — `threedpad_load_buffer` already applies
  `^ 0xF` — so a plain gamepad read as the exact complement of its buttons.
  Beetle folds the inversion into `UpdateInput` instead (`smpc_iodevice.c:201`)
  and additionally masks bits 12 and 13 out of the inverted word; since
  `~x & ~m` is `~(x | m)`, that is the same as forcing those two bits here
- `nyb_source` read the nybble descriptor's argument with a 3-bit mask, but
  `pack_nyb` packs a constant in four bits, so `emit_nyb_const(s, 0xF)` — the
  `F` of the `F1 02` report header — silently became `0x7` and the `id1 == 0xB`
  branch reported `0x71` as its header. That branch is the generation that does
  not negotiate, and nothing but a plain gamepad ever reached it, so no
  scenario caught it

The last three were found by one scenario, `intback_gamepad`, and they are
worth reading as a sequence: fixing the first two left the report still wrong,
because the third was hiding behind the `0x7` header. It stays out of both
columns — `sega_saturn_task` sends `DEV_SATURN_DIGITAL` to `default: BADTYPE`
(`sega_io.c:845-846`, `:883-884`), so BlueRetro never serves a standalone
Saturn Digital Gamepad and nothing in this tree can derive its report.
Recording Beetle's bytes would have made the authoritative column a copy of
the reference one. Its trace is recorded at
`sim/traces/intback_gamepad.beetle` and matches byte for byte:

```sh
./build/tb ours intback_gamepad
```

## Build and test

```sh
./sim/build.sh          # needs BEETLE_ROOT, see below
./sim/differ.sh         # all scenarios
DIFF_VERBOSE=1 ./sim/differ.sh   # with diffs
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
