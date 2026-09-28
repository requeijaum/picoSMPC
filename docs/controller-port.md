# The Saturn controller port, from hardware

The most useful reference in this project is **not** an emulator. It is
`reference/bluRetro/` — BlueRetro's `main/wired/sega_io.c`, which drives a real
Saturn controller port with a real ESP32, in production, and is validated
against real consoles. It answers several questions no emulator in the tree
answers, and it *disagrees* with Mednafen about some of them.

Apache-2.0. Not built, not linked, not shipped.

## The pins

Seven per port. Two ports. From `sega_io.c`:

```c
#define P1_TH_PIN 35   TH   request strobe, driven by the SMPC
#define P1_TR_PIN 27   TR   clock,          driven by the SMPC
#define P1_TL_PIN 26   TL   ack,            driven by the peripheral
#define P1_R_PIN  23   R    data bit 3
#define P1_L_PIN  18   L    data bit 2
#define P1_D_PIN   5   D    data bit 1
#define P1_U_PIN   3   U    data bit 0
```

**The four data lines are named R, L, D and U** — after the directions they can
drive — not `D0`-`D3` as every emulator and reverse-engineering write-up calls
them. Abrasive's `chat_padulator` names its AVR pins `P_D0`..`P_D3`, which is
where the D0-D3 convention comes from; it is a reasonable mnemonic, but it is
not the name on the netlist. Bit order, from `tx_nibble`:

```c
for (i = SIO_R, mask = 0x8; mask; mask >>= 1, i++)   /* R=8, L=4, D=2, U=1 */
```

so `D3..D0` in the SMPC manual is `R,L,D,U` on the connector.

**There is no SC or RMD line.** Every emulator models one, and Mednafen
passes bits 5-7 through as if it did. The Saturn-versus-Mega-Drive distinction
is carried in the *terminator nybble* of each transfer instead — see below.
This is the single biggest correction this reference provides.

## Who drives what

| Line | Direction | Idle |
|---|---|---|
| `TH` | SMPC → peripheral | high (pull-up), peripheral takes a **falling-edge** interrupt |
| `TR` | SMPC → peripheral | high |
| `TL` | peripheral → SMPC | high |
| `R L D U` | peripheral → SMPC | high |

Peripheral-side configuration, `sega_io.c:1070-1117`: TH is an input with a
pull-up and a negative-edge interrupt; TR is an input with a pull-up;
**TL and the four data lines are outputs.** The SMPC is unambiguously the
master, and `TH` is a *request* rather than a clock — one falling edge starts
a whole transfer, which the peripheral then services by polling `TR`.

## The handshake

`twh_tx()` is the peripheral side of one transfer, and it is the clearest
statement of the protocol anywhere:

```c
while (TR is HIGH)   { if (TH went high || timeout) abort; }   /* wait for clock */
tx_nibble(data[i] >> 4);                                     /* drive high nybble */
delay_us(ack_delay);
TL = state; state ^= 1;                                      /* ack */
while (TR is LOW)    { if (TH went high || timeout) abort; }   /* wait for clock */
tx_nibble(data[i] & 0xF);                                     /* drive low nybble */
delay_us(ack_delay);
TL = state; state ^= 1;                                      /* ack */
/* then repeat forever, re-sending the last byte */
```

So: the SMPC strobes `TH`, then toggles `TR`; the peripheral drives each data
nybble and toggles `TL` in response. The data is **held** while `TR` is in a
given state and sampled on the transition. `TL` is the peripheral's own toggle,
so the SMPC can tell "nybble presented" from "nybble stale" — which is why
Mednafen's `JR_WAIT(JR_BS & 0x10)` polls it.

The trailing `while (1)` matters: after the packet the peripheral keeps
answering with the last byte, so a host that over-clocks gets the terminator
repeated rather than garbage.

## `ack_delay`: a real timing number

```c
twh_tx(port, buffer, 4, 0);      /* digital pad   */
twh_tx(port, buffer, 8, 0);      /* analog pad    */
twh_tx(port, buffer, 5, 14);     /* SEGA mouse    */
twh_tx(port, buffer, 6, 0);      /* keyboard      */
twh_tx(port, buffer, ..., 0);    /* multi-tap     */
```

**14 µs for the mouse, 0 for everything else.** The mouse needs the settle time
because building its report means latching a delta accumulator; the pads serve a
static buffer. This is the only measured pad-port settle time in the entire
project, and it came from someone measuring hardware rather than reading
documentation.

## Packet formats

Definitive, and mostly disagreeing with Mednafen. `buffer[]` is a byte array;
`twh_tx` sends `data[i] >> 4` then `data[i] & 0xF`.

| Device | Nybble stream |
|---|---|
| Saturn digital pad | `0, 2` … `b0, b1` … `1` |
| Saturn analog pad | `1, 6` … `b0, b1, AX, AY, AR, AL` … `1` |
| SEGA Mouse | `F, F, flags, buttons, X, Y` … `0` |
| Saturn keyboard | `3, 4` … `b0, b1, 0x06, 0x00` … `1` |
| Saturn multi-tap | `4, 1` … `count, 0` … *(per sub-slot: `ID2, size`, data)* … `1` |
| Saturn multi-tap, mouse sub-slot | `E, 3` … `flags/buttons, X, Y` |

Points worth having:

- **The terminator nybble is the device class.** `1` for Saturn-class
  peripherals, `0` for the Sega Mouse and Mega Drive pads. That is the SC/RMD
  distinction, and it lives in the last nybble rather than on a pin.
- **The analog pad's order is `AX, AY, AR, AL`** — right trigger before left.
  This matches Ymir and Mednafen, and is a useful independent confirmation.
- **The mouse is 4 data bytes**: `flags`, `buttons`, `X`, `Y`. Mednafen's mouse
  reports 3, with the flags and buttons packed into one byte. One of the two is
  wrong and BlueRetro is running on hardware.
- **The multi-tap header is `4, 1` then `count, 0`** — the sub-slot count is in
  the *high* nybble of the second byte. Mednafen assembles its `IDTap` as
  `((ID2 & 0xF) << 4) | (JR_BS & 0xF)`, which reads the count from the *low*
  nybble. With BlueRetro's format that yields a count of zero.
- **A mouse behind a multi-tap reports `E, 3`**, not `2, 3`. The low nybble of
  the peripheral-id nybble is the data size; the high nybble is the type, and
  `0xE` is what a mouse behind an adapter uses.

## ID taxonomy

Complete, and the only one of the four. From `sega_io.c:48-80`:

| `ID1` | Device | | `ID2` | Device |
|---|---|---|---|---|
| `0x3` | Sega Mouse | | `0x0` | Saturn digital pad |
| `0x5` | Saturn peripheral (packet mode) | | `0x1` | Saturn analog pad |
| `0x7` | Mega Drive multi-tap | | `0x2` | Saturn pointing device |
| `0xB` | **Saturn Control Pad** | | `0x3` | Saturn keyboard |
| `0xD` | Mega Drive pad | | `0x4` | Saturn multi-tap |
| `0xF` | nothing connected | | `0xE` | legacy / mouse behind a tap |
| | | | `0xF` | nothing connected |

`ID1 = 0xB` for the Saturn Control Pad is independent confirmation of the one
deduced from Mednafen's `ID1 == 0xB` branch — the path that no emulator's
device set actually reaches.

`ID0` is a further half-byte of identification that the peripheral drives on the
data lines *before* the TH edge, in two phases (`id0_lo` then `id0_hi`), so the
SMPC can classify a device without clocking it. The phase values are
32-bit GPIO register masks (`P1_MOUSE_ID0_HI 0xFF79FFD5`, and so on) and are
specific to BlueRetro's pin assignment, so they cannot be lifted directly —
but the *existence* of a two-phase pre-TH identification is a hardware fact
that no emulator models, and it is a plausible explanation for why the ID1
de-scrambling looks the way it does.

## Trigger thresholds

```c
if (axes[TRIG_L] < 0x56) buttons |= BIT(SATURN_L);        /* released */
else if (axes[TRIG_L] > 0x8D) buttons &= ~BIT(SATURN_L); /* pressed  */
```

with the trigger axes' neutral at `0x00`. So press above `0x8D`, release below
`0x56`. That brackets Ymir's `>= 0x8E` / `<= 0x55` to within one count, from
an independent source, and it contradicts the `0x6A` release figure in the
Yabause UT notes — which were measured on a *Mission Stick*, a different
device with neutral at `0x80`. The two are not in conflict; the tables in
`docs/timing-baseline.md` should be read per-device.

## What this reference does *not* give us

- No SMPC. BlueRetro replaces the pad, not the chip; the SMPC in the console
  is real. So there is nothing here about the host bus, the command engine, the
  RTC, or the report sequencer.
- No timing in SMPC-clock terms. `delay_us(14)` is in the peripheral's own
  timebase, not the SMPC's 4 MHz.
- No multi-tap *clocking* — it answers with a pre-assembled buffer, so the
  per-sub-slot handshake (does the SMPC re-interrogate each slot?) is still
  unconfirmed. BlueRetro's format says what the answer *is*, not how the SMPC
  asks for it.
