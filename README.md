# picoSMPC

A behavioural reimplementation of the Sega Saturn's SMPC — the Hitachi
HD404920FS / Sega 315-5744 — as portable C, with a testbench that checks it
against expectations derived from hardware references rather than against
another emulator.

**This is a research artefact, not a product.** The original plan was an
RP2350B drop-in replacement for the chip. No hardware is available, so the
firmware and board phases were abandoned rather than deferred, and what is
here is the model plus the method that validates it. See
[Scope](#scope-and-non-goals) and [Known limitations](#known-limitations)
below — read the second one before relying on any number in this file.

## Status

| Phase | State |
|---|---|
| 0 — references, timing baseline | done |
| 1 — portable core skeleton + golden-model harness | done |
| 2 — command engine, INTBACK, RTC/SMEM | done — command engine, status report, SETTIME, SETSMEM, area code, reset debounce and the peripheral-report path all match |
| 3 — pad protocol, multi-tap, virtual device | done — digital pad, 3D pad in both modes, mouse, and the multi-tap all match the hardware-derived expectations |
| 4 — RP2350B firmware (PIO, timebase) | abandoned — no hardware |
| 5 — RTC/NVRAM/STE on hardware | abandoned — no hardware |
| 6 — hardware | abandoned — no hardware |

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

**12 of 12 scenarios pass the hardware column; 11 of 12 match Beetle.**

| Scenario | State |
|---|---|
| `status_only`, `command_matrix`, `sysres_ckchg`, `settime_smem` | byte-identical on both columns |
| `intback_one_pad`, `intback_analog`, `intback_digital_buttons`, `intback_multitap`, `intback_multitap_p1`, `direct_mode` | byte-identical on both columns |
| `intback_mouse` | passes the hardware column; `INFO` against Beetle on the payload bytes |
| `intback_rtc_oscillator` | outside both columns by design; see the crystal-tolerance note below |
| `intback_gamepad` | outside both columns by design; its recorded trace matches Beetle byte for byte |

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

### Why `intback_rtc_oscillator` is in neither column

`intback_gamepad` is outside both columns because nothing in the tree can
derive a first-generation gamepad's report. `intback_rtc_oscillator` is
outside them for the same reason and the same way: it runs the watch crystal
5 % fast, and a synthetic tolerance has no hardware counterpart, so there is no
source to derive the expected clock from. Writing down the value the model
produced would be recording a measurement as a fact — the exact failure the
`--` policy in `sim/gen_expect.py` exists to prevent.

Both are registered in `tb_scenarios[]` and both run; each has a recorded
trace at `sim/traces/<name>.beetle` to compare against by hand:

```sh
./build/tb ours intback_rtc_oscillator
```

The difference is that this one is meant to diverge. The reference backend
has no crystal tolerance to set, so it reports the nominal time and ours does
not — that gap *is* the test.

## Scope and non-goals

**In scope.** A portable, allocation-free model of the SMPC register file,
command engine and INTBACK report sequencer, plus a testbench that can prove
the model right about *data* and *effect order* from documentation, without a
console.

**Out of scope, and why.**

- *Cycle-accurate timing.* Every delay in the model comes from Mednafen, whose
  clock ratio is known to overflow and run its SMPC 5.67× slow. There is no
  hardware here to measure the true value against, so the timing set is
  **untrusted** and stays that way. See `docs/timing-baseline.md`.
- *Replacing the chip.* Phases 4–6 needed a board and a PIO design against a
  pad-port half-period that no source states. Neither exists.
- *Proving the model complete.* Three of eight device types and none of the
  environment callbacks are covered; see below.
- *A third, cycle-accurate oracle.* Considered and rejected, for the record.
  `Saturn_MiSTer/rtl/Saturn/SMPC/` contains a cycle-accurate HMCS400 core
  together with `smpc.mif` — Sega's SMPC ROM, 2048 words of 10 bits. Pointed
  at a Verilator build and driven through an interface shaped like this one's,
  it would have been the only thing in reach that could have tested the
  timing this project calls untrusted. Rejected because no HDL toolchain is
  installed (no verilator, iverilog, yosys or ghdl), the ROM may not be
  redistributed, and a SystemVerilog shim for the host bus, the pad port and
  the VDP/SCU/sound stubs is a new subsystem rather than a small addition.
  The three-stage peripheral model it contributes was adopted by reading; the
  simulator was not built.

## Known limitations

Read this before trusting any figure above. Every row is checkable.

| Limitation | Where to check it |
|---|---|
| The model is **data-correct, timing-untrusted**. No constant in `core/` was ever measured; all of them are Mednafen's, from a source this project shows is wrong about time. | `docs/timing-baseline.md` |
| The harness's clock argument was **61× wrong and is now fixed** — `tb_smpc_master_clock()` matches `ss.c`, and `intback_rtc_tick` checks it against the NTSC frame rate rather than against the model. But the harness still **cannot test any wall-clock behaviour**: the differ compares no timestamps, by design. | `docs/timing-baseline.md` |
| The harness emits **28 MHz frames while the SMPC sits in its power-on 26 MHz mode** (nothing has issued CKCHG352), so a frame is worth 1.0654 model seconds. Beetle has the same mismatch, so neither column can see it. | `docs/timing-baseline.md` |
| The controller-port **TH half-period is unknown and unknowable offline**. Nothing in `reference/` states it, and there is no hardware capture in the tree. It is the one number a real deployment would need first. | `docs/timing-baseline.md` |
| **Only 3 of 8 device types** are exercised by a scenario: `3dpad`, `mouse`, `gamepad`. `WHEEL`, `MISSION`, `KEYBOARD` and `GUN` are implemented and untested — including `WHEEL`'s hysteresis thresholds, which are among the few genuinely *measured* numbers in the project. | `sim/tb.c`, `core/include/smpc/iodev.h` |
| **No `SmpcEnv` callback is asserted.** All eight are recorded in the trace, and `differ.sh` never reads them, so `MSHON`/`SSHON`/`SNDON`/`CDON`/`NMIREQ`/`RESENAB` are checked only for the command byte they leave in `OREG[31]`. | `sim/differ.sh` (zero references to `EVT`) |
| The differ compares **`OREG` and `SR` only**. `smpc_get_rtc`, `smpc_get_smem` and `smpc_read_reg` have no test at all. | `sim/differ.sh` |
| **Beetle is the only executable oracle for report framing.** MAME is consulted in prose only, and abrasive's HMCS400 work feeds no automated check. A bug inherited from MAME or Mednafen would pass *both* columns. | `docs/smpc-implementations.md` |
| `intback_mouse` has **two bytes with no derivation**, and `intback_gamepad` is in neither column. Both are recorded as `--`, never guessed. | `sim/traces/*.expect` |
| Whether the SMPC **re-interrogates each multi-tap sub-slot** is unconfirmed: BlueRetro answers with a pre-assembled buffer, so it shows *what* the reply is, not *how* the SMPC asks. | `docs/controller-port.md` |
| The port mode echoed in SR is read from **IREG0**, against the manual and now against 4 of the 7 implementations surveyed — including the FPGA core people actually run games on. The model sits on the minority side. | `docs/smpc-implementations.md` §1 |
| The two port modes are **one field copied, not two fields**. If the chip carries a per-port mode, this model cannot produce SR bits 0-1 differing from bits 2-3. Found by reading `Saturn_MiSTer`, undocumented before. | `docs/smpc-implementations.md` §1b |
| The RTC second's **tolerance is now a parameter** (`smpc_set_rtc_oscillator`), but it defaults to 0 ppm — the core oscillator's rate, which is Mednafen's behaviour. A real deployment still has to set it, and the only test of a non-zero value uses a deliberately absurd 5%. | `docs/timing-baseline.md` |
| **No battery, no supply monitor.** The core runs from the host *and* a CR2032, the RTC keeps counting with the console off, and IC25 raises interrupt 0 and wipes the save RAM when the battery is discharged. None of it is modelled; `rtc_valid` is a static flag that measures nothing. | `docs/timing-baseline.md` |
| Pin D0's **16.384 kHz** output — the RTC oscillator divided by two, readable by the host in direct mode — does not exist in the model. | `docs/timing-baseline.md` |

The recurring failure mode behind most of these is worth stating: a code path
with no scenario is a code path where a bit-packing bug can live for months.
The 3-bit `nyb_source` mask sat in the `id1 == 0xB` branch until a single
scenario reached it. Coverage, not correctness, is what this model is short of.

## Layout

```
core/     the model.  Portable C99, no dependencies, no allocation after init.
sim/      testbench, golden-model harness, and the expectation generator.
docs/     timing baseline and architecture notes.
tools/    fetch-reference.sh -- populates reference/ with the third-party trees.
reference/  golden model and reverse-engineering material.  Not built.
```

`reference/bluRetro/` and `reference/smpc-emulator/` are **not** in the
repository — the first is 3 MB of someone else's history, and the second
contains Sega's SMPC ROM and the Saturn Service Manual, which are not ours to
redistribute. `tools/fetch-reference.sh` clones both from their canonical
URLs. See `NOTICE.md` for provenance and licensing.

## Build and test

```sh
tools/fetch-reference.sh       # once: clones the excluded reference trees
```

`BEETLE_ROOT` must point at a `beetle-saturn-libretro` checkout; it defaults
to `~/projects/saturn_emulator_for_chinese_handhelds/beetle-saturn-libretro`.
`arm-none-eabi-gcc`, `cmake` and `ninja` are already installed. `pico-sdk` and
`picotool` are not, and are not needed — phase 4 was abandoned.

Everything, in one command:

```sh
python3 sim/gen_expect.py --check && ./sim/build.sh && ./sim/differ.sh
```

Individually:

```sh
./sim/build.sh          # needs BEETLE_ROOT, see below
./sim/differ.sh         # all scenarios
DIFF_VERBOSE=1 ./sim/differ.sh   # with diffs
./build/tb ours         # our core only, all scenarios
./build/tb beetle       # the golden model only
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

## Four findings worth knowing about up front

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

`core/`, `sim/`, `docs/` and `tools/` are our own work and are MIT licensed —
see [LICENSE](LICENSE). Full provenance for every third-party part, including
the two trees deliberately *not* redistributed, is in
[NOTICE.md](NOTICE.md), with the licence texts in `reference/licences/`.

The short version: the golden model in `reference/beetle/` is Mednafen's,
GPL-2.0-or-later, unmodified and unlinked into anything we ship. The
controller-port reference in `reference/bluRetro/` is BlueRetro's, Apache-2.0.
The reverse-engineering material in `reference/smpc-emulator/` is abrasive's,
CC-BY, and is where the hardware facts in `docs/` come from. Sega's SMPC ROM
and the Saturn Service Manual are Sega's, and are not in this repository at
all.
