# Architecture

## What this is

A behavioural reimplementation of the Sega Saturn's SMPC — the Hitachi
HD404920FS / Sega 315-5744, an HMCS400 4-bit MCU running at 4 MHz — with a
testbench that checks it against expectations derived from hardware references.
The original plan was an RP2350B drop-in replacement for the chip; no hardware
is available, so that was abandoned rather than deferred, and the model plus
the method that validates it is the artefact. See the top-level README for the
scope and what is deliberately out of it.

The port is not a serial device. The SMPC is an 8-bit parallel slave on the
SH-2 bus, and it is the *master* on the controller port. Both of those
relationships have to be reproduced at the pin, not approximated over a
link.

## Three layers

```
core/     portable C99, no dependencies, no floating point, no allocation
          after init.  This is the model.
sim/      native testbench.  Runs the scenarios in tb.c twice: once against
          our core, once against Mednafen's SMPC, and reports two columns --
          the hardware-derived expectations and the Mednafen recording.
```

There is no third layer. A firmware layer for an RP2350B was planned and
dropped; `core` never talked to hardware and does not now, which is why it is
portable C and why it still builds for a microcontroller without being built
for one.

`core` never talks to hardware. Everything it does outside its own register
file goes through `SmpcEnv` in `core/include/smpc/smpc.h`, so the same object
code runs in the testbench and on the MCU, and the acceptance test is that the
two produce identical traces.

## The host bus

The SMPC's six address pins are the SH-2's **A2..A7**, so the register index
is `(sh2_byte_address & 0x7F) >> 1` and only odd byte addresses decode. That
shift is bus wiring, not SMPC behaviour, so the testbench applies it and both
models provably see the same index. This is worth writing down because it is
easy to assume the SMPC sits on A1..A6 and get every register wrong.

| SH-2 address | Register |
|---|---|
| `0x01`..`0x0D` | IREG0..6, write-only command arguments |
| `0x1F` | COMREG — write starts a command; reads back as OREG31 |
| `0x21`..`0x5F` | OREG0..31 |
| `0x61` / `0x63` | SR / SF |
| `0x75`..`0x7F` | PDR1/2, DDR1/2, IOSEL, EXLE |

A read of an address with no register returns the last value written to the
port. The data bus is open, and software depends on it: bit 7 of a PDR read
and bits 7..1 of an SF read come from there.

## The controller port

The SMPC drives `TH` (strobe) and `TR`; the peripheral drives `TL` and a 4-bit
data nybble. The SMPC is the master. So this project has to contain **both**
sides of the handshake: `core/src/smpc.c` is the master, `core/src/iodev.c` is
the peripheral. That is not redundancy — it is what lets a USB HID controller
substitute for a physical one, which is the feature that actually addresses
the burnout problem this project exists to solve.

Two details in there are load-bearing and easy to get backwards:

- **The DDR polarity is inverted from what the name suggests.** A 1 bit means
  the SMPC *drives* the line; a 0 releases it to float high.
- **`IOSEL` is a pin mux, not a mode bit.** There is one PDR and one DDR. Index
  0 of the port's register file holds what the SMPC's own driver is putting on
  TH/TR; index 1 holds what the SH-2 last wrote. A host write to PDR is
  therefore *inert* while the SMPC is driving the port. A single shared
  register cannot express that, and titles that poke the port directly rely on
  it.

## INTBACK is a nybble DMA, not a byte DMA

OREG is physically 32 bytes of **64 nybbles**, walked by a 6-bit write pointer,
one nybble per ~5.25 µs. Every HLE in existence models OREG as a flat byte
array that the SMPC fills in one go. That is wrong in a way games notice,
because the SH-2 reads each byte while the SMPC is still overwriting the next
one, and because the SMPC stops at every 32-nybble boundary to pulse the
interrupt and wait for the host to say "continue" or "break".

So the peripheral half of an INTBACK is not straight-line code. It has loops
whose trip count is only discovered over the wire, and it suspends constantly.
`core/src/smpc.c` emits it as a small instruction stream and interprets it, so
suspending is literally "return, with the program counter left where it is".

The two bugs that design was chosen to make impossible — a suspension point
that forgets where to resume, and a `break` that only leaves the enclosing
`do-while` — are the two that actually happened while writing it. Both are
documented at their call sites.

## The `EAT_THEN` rule

Delays are consumed with one macro. Two rules, both learned the hard way:

- The escape is a `goto`, not a `break`. `break` only leaves the `do-while`
  the macro is wrapped in, and the rest of the case then runs with a phase
  that has already moved on.
- The resume phase is **this** case, never the case it is waiting for.
  Resuming into the target skips everything between the wait and the phase
  assignment — which is exactly how SETTIME and SETSMEM ended up silently
  dropping their writes.

## Testing

`sim/differ.sh` replays each scenario through both models and compares
register contents and the sequence of side effects. Timestamps are *not*
compared: the golden model's clock ratio overflows its 32-bit accumulator and
runs 5.67× slow, so its timing is wrong by construction. See
`docs/timing-baseline.md`.

The harness counts System Manager interrupts and services the INTBACK
continue handshake the way the BIOS handler does. This matters more than it
looks: without it both models hang identically, which reads as a pass.
