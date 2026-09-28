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
netlist. Both are in the abrasive clone's `reference_docs/`.
