# Golden model: Mednafen's SMPC

`smpc.c` and `smpc_iodevice.c` are **unmodified copies** of

    beetle-saturn-libretro/mednafen/ss/smpc.c
    beetle-saturn-libretro/mednafen/ss/smpc_iodevice.c

They are compiled with `-w` against the real Mednafen headers and linked
against `sim/golden/shim.c`, a stub of everything they touch outside the
SMPC (SCU, VDP, sound, SH-2, the event list). This turns the reference
implementation into a plain function library that the testbench in `sim/` can
drive, so our core and the reference see byte-identical stimulus.

Why this model and not another:

- **Beetle** is the only implementation in the tree that models the
  controller port at the pin level, including the TH/TR/TL half-period
  handshake and the multi-tap. Ymir and Yabause both short-circuit the
  peripheral bus.
- It is **cycle-based**: it counts SMPC clocks, so the delays it uses are
  meaningful as *durations* even though its clock ratio is broken (see
  `docs/timing-baseline.md`).
- Ymir is better on peripheral *data* formats — it has hardware-verified test
  failures logged against specific games — and `core/src/iodev.c` is ported
  with that in mind.
- `abrasive/smpc-emulator` is the authority on the pinout and the internal
  architecture, not on cycle timing. It is cloned to `reference/` for the
  hardware documentation; it is not built.

`sim/build.sh` needs `BEETLE_ROOT` to point at a `beetle-saturn-libretro`
checkout. It defaults to
`~/projects/saturn_emulator_for_chinese_handhelds/beetle-saturn-libretro`.
