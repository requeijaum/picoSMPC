# Four SMPC implementations, compared

There are four independent SMPC models in this tree, plus a hardware
reference. They disagree — about register maps, about the status report, about
the INTBACK handshake. This is the comparison, and what the project does about
each disagreement.

| Source | Where | Approach |
|---|---|---|
| **Beetle / Mednafen** | `reference/beetle/` | the only cycle-based model; the golden oracle |
| **Ymir** | `ymir-muos-h700/ymir-src` | best peripheral *data*, hardware-verified fixes logged |
| **Yabause** | `yabause/yabause/src/smpc.c` | flat, timing-hacked; no RTC at all |
| **Kronos' Yabause** | `Kronos/yabause/src/sys/smpc/` | a substantially better INTBACK handshake |
| **MAME** | `reference/mame_smpc.cpp` | table-driven, no pad protocol at all |
| **5thPlanet** | `5thPlanet/crates/saturn/src/smpc.rs` | modern Rust, cites MAME, self-labels unverified |
| **BlueRetro** | `reference/bluRetro/main/wired/sega_io.c` | real hardware, peripheral side — see `controller-port.md` |

`seta-gx` and `yabause-vita` are Yabause forks with no meaningful SMPC changes.
`erings` has no SMPC at all (HLE BIOS, CD block only).

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

**MAME, Kronos, and 5thPlanet all read `IREG0 >> 4`.** Beetle, Ymir, and
upstream Yabause read IREG1 (bits 4–5 for port 1, 6–7 for port 2), which is
what the SMPC manual says.

```c
// MAME
m_pmode = m_intback_buf[0]>>4;
sr_set(0x80 | m_pmode);
// Kronos
m_pmode = (SmpcRegs->IREG[0]>>4);
```

Three-against-three on the vote, but MAME's driver is the oldest and the most
hardware-validated of the three on the IREG0 side, and the IREG1 side is
trivially self-consistent (a copy of an existing field). **Adopted: IREG0.**
Flagged in the code as unresolved.

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
| 5thPlanet | the three-stage `intback_stage` model, pending |

The pattern worth noticing: **the newer and less famous implementations are
the more careful ones.** Beetle has the best timing and the worst field
documentation; MAME has the best field documentation and no timing at all.
Neither is authoritative, and the useful thing to do is keep both and diff.
