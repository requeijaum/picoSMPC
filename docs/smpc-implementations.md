# The SMPC implementations in reach, compared

Several independent SMPC models are in reach from here, plus a hardware
reference. They disagree — about register maps, about the status report, about
the INTBACK handshake. This is the comparison, and what the project does about
each disagreement.

The list is seven models plus the peripheral-side reference: Beetle/Mednafen,
Ymir, Yabause and its Kronos fork, MAME, 5thPlanet, and Saturn_MiSTer. Four of
them are ports of each other or cite each other, so the number of *opinions*
is smaller than the number of rows — the interesting part is which
implementations arrived at which conclusion independently.

> **The "Where" column below is not resolvable inside this repository.** Only
> `reference/beetle/`, `reference/mame_smpc.cpp` and `reference/bluRetro/` ship
> here; the others were read from sibling checkouts in a separate tree.
> Upstream URLs: Ymir `https://github.com/ichard26/Ymir`,
> Yabause `https://github.com/requeijaum/yabause`,
> Kronos `https://github.com/FCare/Kronos`,
> 5thPlanet `https://github.com/hiroshiyui/5thPlanet`,
> MiSTer `https://github.com/MiSTer-devel/Saturn_MiSTer`.

| Source | Where | Approach |
|---|---|---|
| **Beetle / Mednafen** | `reference/beetle/` | the only cycle-based model; the golden oracle |
| **Ymir** | `ymir-muos-h700/ymir-src` | best peripheral *data*, hardware-verified fixes logged |
| **Yabause** | `yabause/yabause/src/smpc.c` | flat, timing-hacked; no RTC at all |
| **Kronos' Yabause** | `Kronos/yabause/src/sys/smpc/` | a substantially better INTBACK handshake |
| **MAME** | `reference/mame_smpc.cpp` | table-driven, no pad protocol at all |
| **5thPlanet** | `5thPlanet/crates/saturn/src/smpc.rs` | modern Rust, cites MAME, self-labels unverified |
| **Saturn_MiSTer** | `Saturn_MiSTer/rtl/Saturn/SMPC_HLE.sv` | FPGA core; the one people actually run games on |
| **BlueRetro** | `reference/bluRetro/main/wired/sega_io.c` | real hardware, peripheral side — see `controller-port.md` |

`seta-gx` and `yabause-vita` are Yabause forks with no meaningful SMPC changes.
`erings` has no SMPC at all (HLE BIOS, CD block only).

### A note on Saturn_MiSTer

It gets its own note because it is the only tree here with **two**
implementations of the chip. `rtl/Saturn/SMPC/` is a cycle-accurate HMCS400
core plus `SMPC.sv`; `rtl/Saturn/SMPC_HLE.sv` is a 37 KB HLE. Both appear in
`Saturn.qip`, but only the HLE is instantiated — `Saturn.sv:790` declares
`SMPC_HLE SMPC`. The cycle-accurate one is dormant.

That makes the HLE the model with the strongest claim to working: it is the one
implementation here under continuous test by people playing Saturn games on
FPGA — its own README claims "many games tested over the course of the
development", and that is the claim, not this document's. Where it disagrees
with the emulators that is not a tie — and two of the places it disagrees, the
port mode and the empty-port report byte, are divergences this project had
previously scored differently.

Its three peripheral families match ours exactly: `0xB` (Control Pad), `0x3`
(Mouse) and `0x5` (TL-negotiated), with `0xA`/`0xF` folded into "nothing". No
wheel, mission stick, keyboard or gun. **It has no multi-tap at all** —
`SMPC_HLE.sv:1216` is `end else begin //TODO: multitap` — so the multi-tap
bugs fixed in this project's last commit sit beyond anything it models, and it
is no oracle for that path.

Its RTC is `input [64:0] EXT_RTC`, fed from the MiSTer's own clock. There is
no `32768` anywhere in the file: the console's battery-backed oscillator is
absent by construction, which is a weaker position on that property than this
model's.

It carries no `LICENSE` file and no credits. MiSTer cores are conventionally
GPL-3.0 and the MiSTer build system requires GPL-compatible cores, but this
repository does not say so — so compare and cite, do not copy, until that is
settled.

> **MAME and the sibling checkouts feed no automated check.** This document is
> prose, and a claim in it is not a test. The only executable oracle for report
> framing in the whole project is Beetle, which is also the second data column
> in `sim/differ.sh` — so a defect inherited from Mednafen or MAME would pass
> *both* columns. That is the structural weakness of the methodology, and it
> is why the sections below end by saying how each divergence was settled.

## Where they agree

Worth stating, because it is most of the model:

- The register map, and the odd-address-only decode. Kronos states it
  explicitly: `if(!(addr & 0x1)) return;`
- `(addr & 0x7F) >> 1` as the register index.
- OREG is 32 bytes; OREG31 echoes the command.
- SR bit 7 distinguishes a status report (0) from a peripheral report (1), bit
  6 is PDL, bit 5 is NPE.
- INTBACK is two phases, gated on IREG0 (status) and IREG1 bit 3 (peripheral).
- Commands 0x00–0x1A with the same meanings. All of them have
  `NETLINKON`/`NETLINKOFF` (0x0A/0x0B) commented out or unimplemented; only
  abrasive's ROM disassembly shows they exist.
- The open data bus: an undecoded read returns the last written value.
- Peripheral data is collected **outside** vblank. Beetle's `JR_WAIT(!vb)` and
  Kronos's `intback_wait_for_vblankout` agree, and it is what Virtua Racing
  requires.

## Where they disagree

### 1. The port mode in SR bits 0–3: IREG0 or IREG1?

**MAME, Kronos, and 5thPlanet all read `IREG0 >> 4`.** Beetle, Ymir, upstream
Yabause and Saturn_MiSTer read IREG1 (bits 4–5 for port 1, 6–7 for port 2),
which is what the SMPC manual says.

```c
// MAME
m_pmode = m_intback_buf[0]>>4;
// Kronos
m_pmode = (SmpcRegs->IREG[0]>>4);
// Saturn_MiSTer
PMD[0] <= IREG[1][5:4];
PMD[1] <= IREG[1][7:6];
```

**Adopted: IREG0.** But the argument that carried it has weakened, and it is
worth being exact about why.

The original reasoning was "three against three, and MAME's driver is the
oldest and most hardware-validated on the IREG0 side, while the IREG1 side is
trivially self-consistent because it is a copy of an existing field". Saturn_MiSTer
breaks both halves of that. It is a fourth vote for IREG1, and it is not a
like vote: MiSTer is the one implementation in this document that people run
Saturn games on, so its reading is under continuous test by the only oracle
that cannot argue back. And it does *not* copy a field — it has two
independent ones, so "trivially self-consistent" was never a property of the
IREG1 reading, it was a property of our implementation of it.

Against that: MAME's driver is still the most hardware-validated *source* on
the IREG0 side, and the manual says IREG1. So the honest summary is that this
model sits on the minority side, and that the strongest practical evidence
available offline points the other way. It remains flagged in the code as
unresolved. If one thing were to be measured on real hardware, it should be
this: write a known IREG1 and see which bits of SR change.


### 1b. Are the two port modes independent, or one field echoed twice?

This is not a documented divergence; it is a disagreement about the *shape* of
the feature, and it only became visible once an FPGA core was read alongside
the emulators.

```c
/* this model, core/src/smpc.c */
s->jr.mode[0] = (uint8_t)((s->ireg[0] >> 4) & 3);
s->jr.mode[1] = (uint8_t)(s->jr.mode[0] & 3);   /* a copy */

/* Saturn_MiSTer */
PMD[0] <= IREG[1][5:4];
PMD[1] <= IREG[1][7:6];                        /* two independent fields */
```

They cannot both be right. If the SMPC really carries a per-port mode, then SR
bits 0-1 and 2-3 can differ, and this model is structurally unable to produce
that — every INTBACK it builds echoes the same value twice. If the SMPC carries
one mode, MiSTer is reading bits 6-7 for a mode that does not exist per-port,
and its SR bits 2-3 are reading whatever else lives there.

Which way it goes is entangled with §1 above: the copy in our code is only
necessary *because* both modes come from one field. If §1 is resolved in favour
of IREG1, the per-port question becomes unavoidable.

**No source in the tree settles it.** The manual's Parallel I/O Registers
section would, and so would a console. Until then the copy stays, because it is
what the adopted reading of §1 implies — but it is a choice with a shape
behind it, not an obviousness, and it is listed as an open question in the
README.

### 2. Is OREG pre-filled with 0xFF?

MAME fills OREG16..30 and calls them "undefined"; Kronos fills OREG0..30.
Beetle, Ymir, Yabause and this project leave them at their reset value.

**Not adopted.** It regresses against the only oracle this project can test
against automatically, and the conflict is unresolvable without hardware. If
the pre-fill is real, the visible symptom is a short report reading `00` where
it should read `FF` — recorded here so it is a known, checkable hypothesis
rather than a silent choice.

### 3. SF semantics after the status report

Beetle clears SF at the end of every command. Kronos sets it to
`(IREG[1] & 0x8) != 0` after the status report — i.e. SF stays busy only when
peripheral data was requested. MAME is explicit: `sf_ack(false)` at the end of
the status phase.

**Kronos and MAME agree, and it is the more specific behaviour. Not yet
adopted** — it changes the `status_only` scenario's timing and needs the
interrupt count checked before committing.

### 4. SF is read-modify-write

Only Kronos models it: `SmpcRegs->SF &= val` on a write to 0x63, so writing 0
clears the busy flag and writing 1 leaves it. Nobody else does, and all of them
treat a write as "SF = 1, start a command".

Abrasive's `HARDWARE.md` is independent support: the HMCS400's
interrupt-control registers are "only accessible using bit modification
instructions (SEM/SEMD, REM/REMD, TM/TMD)", so a partial write to SF is a bit
clear by construction. **Adopted.**

### 5. The OREG10 bitfield

MAME is the only one that documents the bits:

```
0-11 -1-- unknown
-x-- ---- VDP2 dot select
---- x--- MSHNMI
---- --x- SYSRES
---- ---x SOUNDRES
```

and sets `0x34 | (dotsel << 6)`. Beetle uses
`0x24 | dotsel<<6 | slaveSH2<<4 | 0x08 | 0x02 | soundCPU`, which is `0x2E` at
reset against MAME's `0x34` — they disagree in bit 5 (MAME 1, Beetle 0), bit 4
(MAME 1 fixed, Beetle the slave-SH2 state), bit 3 (MAME 0, Beetle MSHNMI=1),
bit 2 (MAME 1, Beetle "system reset state"=1), and bit 1 (MAME 0, Beetle
SYSRES=1).

OREG11 is the same story: MAME hardcodes `0 << 6` for CDRES, Beetle uses
`(cd_on ? 0x40 : 0) | 0x02`. **Unresolved.** Beetle wins by default because it
is the testable oracle, but MAME's field-level documentation is much more
trustworthy than Beetle's bit soup, and this is a prime candidate for a
measurement once hardware exists.

### 6. Whether the INTBACK continues through SF or through IREG0

Beetle watches IREG0 bit 7 (and 0x40 for break). Kronos also watches IREG0 but
*additionally* re-arms SF on a continue (`SmpcRegs->SF = 1`) and gates the
whole thing on `firstPeri != 0` — the host cannot continue before a peripheral
report is actually in flight. 5thPlanet models a three-stage counter
(`intback_stage` 1 → 2 → 0).

**Kronos and 5thPlanet are more careful.** The `firstPeri` gate is the
behavioural part that matters: a stray IREG0 write during a status report must
not be able to start a peripheral transfer.

### 7. Aborting an INTBACK that runs out of time

Only Kronos: `SmpcINTBACKEnd()`, called at end of vblank, aborts with
`SF = 0` and *no interrupt*. This is the path Discworld depends on, and it is
the cleanest statement of the timeout anywhere. Beetle has the abort but emits
the interrupt anyway.

## What this project takes from each

| From | Adopted |
|---|---|
| Beetle | the whole command engine, the 32-nybble OREG walk, all cycle constants, the open bus, the pad-port pin mux |
| Ymir | peripheral report *formats*, the analogue thresholds, the finicky-games regression list |
| Kronos | SF read-modify-write; the `firstPeri` gate and the no-interrupt abort, pending |
| MAME | the port mode from IREG0; the OREG10 field documentation, pending |
| BlueRetro | the entire pad protocol and packet formats — see `controller-port.md` |
| 5thPlanet | the three-stage `intback_stage` model — now corroborated |
| Saturn_MiSTer | the per-port independence of the mode fields, *not* adopted (see §1b); nothing else, on the evidence available offline |

The pattern worth noticing: **the newer and less famous implementations are
the more careful ones.** Beetle has the best timing and the worst field
documentation; MAME has the best field documentation and no timing at all.
Neither is authoritative, and the useful thing to do is keep both and diff.

## How each divergence was settled

"Adopted" above says what the model does. It does not say how much the choice
is worth, and the two are not the same. The honest categories:

| Category | Meaning | Which divergences |
|---|---|---|
| **Tested** | a scenario in `sim/tb.c` exercises it, and the derived expectation in `sim/traces/*.expect` is checked on every run | the 32-nybble OREG walk; status block layout; pad packet formats and wire polarity; the nybble descrambler; multi-tap sub-slot cursor; the `id1 == 0xB` and `0x3/0x5` report branches; the unmatched-id arm |
| **Prose only** | decided by reading, with no automated check behind it | the OREG10 field documentation; Kronos's `firstPeri` gate; the OREG 0xFF pre-fill (deliberately *not* adopted) |
| **Open** | sources disagree and no test can currently say who is right | the port mode register (IREG0 vs IREG1, now 3 against 4 with the FPGA core on the IREG1 side); whether the two port modes are independent (§1b); the mouse payload length; the multi-tap count nybble (high per BlueRetro, low per Beetle); whether the pad master re-interrogates each sub-slot |

Note the asymmetry in the port mode, because it is easy to misread: a
scenario *does* exercise the mechanism — `direct_mode` and the SR dumps prove
this model reads `IREG0 >> 4` and echoes it, and that it would notice if it
stopped. What is untested is whether that is the *right register*, which is
why it sits in "Open" and not in "Tested". A passing test of a contested
choice is not evidence for the choice.

The 5thPlanet three-stage `intback_stage` has left the "prose only" row and is
not listed above at all: Saturn_MiSTer models the same three peripheral phases
(`CS_INTBACK_PERI`, `PERI2`, `PERI3`) in SystemVerilog, having arrived at them
independently. Two implementations converging on a non-obvious structure is
evidence, though not proof, and it is still not something the harness checks.

Everything in the "Open" row is where this project is weakest, and none of
it is a detail: the port mode decides what SR reports, and the pre-fill and
the count nybble both change bytes that `sim/gen_expect.py` currently records
as `--`. A reader should treat the "Tested" row as the project's actual claim
and the other two as open questions it has documented but not settled.

