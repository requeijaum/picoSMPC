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

The authoritative documents are `ST-169-R1-072694` (the SMPC manual) and page
17 ("Main C/B - 5/6") of the Saturn Service Manual, which carries the IC9
netlist. Both are in the abrasive clone's `reference_docs/`.
