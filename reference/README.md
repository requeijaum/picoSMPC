# Reference material

Nothing in here is built or shipped.

- `beetle/` — Mednafen's SMPC, unmodified, used as the golden model. See
  `beetle/README.md`.
- `smpc-emulator/` — abrasive's HMCS400 core + SMPC ROM emulator (CC-BY),
  cloned from <https://codeberg.org/abrasive/smpc-emulator>. Not in git; clone
  it with:

      git clone https://codeberg.org/abrasive/smpc-emulator reference/smpc-emulator

  The valuable parts are `HARDWARE.md` (the internal memory map and the four
  interrupts) and `NOTES` (the die analysis, the pad port's pin mapping, and
  the recovered command dispatch table). The `dump/` directory contains
  Sega's SMPC ROM: it is **not** needed for an HLE and must not end up in a
  firmware image.

- `bluRetro/` — BlueRetro's `main/wired/sega_io.c`, a working Saturn
  controller-port implementation driving real hardware, and `main/adapter/
  wired/saturn.c`, which defines the report packets it serves. Clone it with:

      git clone --depth 1 https://github.com/darthcloud/BlueRetro reference/bluRetro

  This is the most valuable reference in the project, and the one that is
  least like an emulator: it sits on the *peripheral* side of the port, so it
  answers questions about the pins, the handshake and the packet formats that
  no emulator can, and it contradicts Mednafen on several. See
  `docs/controller-port.md`.

The authoritative documents are `ST-169-R1-072694` (the SMPC manual) and page
17 ("Main C/B - 5/6") of the Saturn Service Manual, which carries the IC9
netlist. **Neither is in the abrasive clone** — earlier revisions of this file
said they were. What `reference_docs/` actually contains is the 1988 Hitachi
HMCS400 Series Handbook, one Hitachi datasheet, and a large die trace. The two
authoritative documents have to be fetched separately, and abrasive's own
README links them:

- <https://antime.kapsi.fi/sega/files/ST-169-R1-072694.pdf>
- <https://segaretro.org/images/4/4e/Sega_Service_Manual_-_Sega_Saturn_%28PAL%29_-_013-1_-_June_1995.pdf>

Both are Sega's and are not redistributed here.

## Not in this repository: the SMPC ROM

A sibling checkout of `MiSTer-devel/Saturn_MiSTer` carries
`rtl/Saturn/SMPC/smpc.mif` — Sega's SMPC ROM, `WIDTH=10; DEPTH=2048`, the 2 048
words abrasive's disassembler is written for. It is the one piece of material
this project has wanted and could not otherwise get: abrasive's Codeberg
repository does not host it, which is why `dump/` is empty there and why the
HMCS400 cannot be run from that tree alone.

It is recorded here as a fact, not as a dependency. It is Sega's, it is not
redistributed here, and nothing in `core/` or `sim/` needs it. Building a
cycle-accurate oracle around it was considered and rejected — see the
non-goals in the top-level README.
