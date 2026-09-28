# Notice

`picoSMPC` — an HLE model of the Sega Saturn's SMPC — is MIT licensed; see
[LICENSE](LICENSE). This file records where the parts of it that are *not* our
own work came from, and under what terms they may be redistributed.

## Our own work

| Path | What it is | Terms |
|---|---|---|
| `core/` | the SMPC model, portable C99 | MIT |
| `sim/` | testbench, golden-model harness, expectation generator | MIT |
| `docs/` | architecture, timing baseline, controller port, implementation survey | MIT |
| `tools/` | `fetch-reference.sh` | MIT |

The model was written from documentation, measurement and comparison. It is not
derived from, and does not embed, any of Sega's SMPC ROM or microcode.

## Third-party material, redistributed

These are unmodified excerpts kept in the repository because the test harness
reads them directly. Full licence texts are in `reference/licences/`.

| Path | Upstream | Terms |
|---|---|---|
| `reference/beetle/smpc.c`, `smpc.h`, `smpc_iodevice.c` | Mednafen Sega Saturn module, <https://github.com/libretro/mednafen-ss> | GPL-2.0-or-later — `reference/licences/GPL-2.0.txt` |
| `reference/mame_smpc.cpp` | MAME, <https://www.mamedev.org/> | LGPL-2.1-or-later — `reference/licences/LGPL-2.1.txt` |
| — | BlueRetro, <https://github.com/darthcloud/BlueRetro> | Apache-2.0 — `reference/licences/Apache-2.0.txt` |

The Mednafen files carry their own header, which states *"GNU General Public
License ... either version 2 of the License, or (at your option) any later
version"*, and the `beetle-saturn-libretro` checkout they came from ships a
GPL-2.0 `COPYING`. `reference/licences/GPL-2.0.txt` is the canonical text of
that licence; the checkout's own `COPYING` differs from it only in whitespace.
The GPL-3.0 text is deliberately **not** included: nothing here is
GPL-3.0-only, and shipping a licence the code is not under invites the wrong
reading.

`reference/beetle/` is a three-file extract of an upstream GPL-3.0 project,
copied verbatim and **not modified**. It is present so that
`sim/gen_expect.py` can cite exact line numbers in a real implementation, and
so the two columns of `sim/differ.sh` can be checked against something other
than ourselves. It is compiled into the test binary and **is not linked into
anything this project delivers**; the model in `core/` has no dependency on it
and is not a derivative work of it. Nothing in `core/` is copied from it — the
defects it revealed were found by *differing* against it, and are documented
in the README.

`reference/mame_smpc.cpp` is likewise a verbatim extract, kept for the
implementation survey in `docs/smpc-implementations.md`. It is not compiled.

## Third-party material, deliberately not redistributed

Two trees are fetched on demand by `tools/fetch-reference.sh` and are listed in
`.gitignore`. They are not in this repository and must not be committed to it.

| Path | Upstream | Terms | Why it is excluded |
|---|---|---|---|
| `reference/bluRetro/` | BlueRetro | Apache-2.0 | 3 MB of somebody else's history; only `main/wired/sega_io.c` is ever read |
| `reference/smpc-emulator/` | abrasive, <https://codeberg.org/abrasive/smpc-emulator> | CC-BY | `reference_docs/` carries a 42 MB vendor handbook and a 24 MB die trace. Somebody else's documents and megabytes of their repository history are not ours to redistribute. |

Both carry attribution-only obligations, which this file and
`reference/README.md` discharge. `tools/fetch-reference.sh` clones each from
its canonical URL, so a reader can obtain the exact tree this project was
written against without it weighing down the repository.

`reference/smpc-emulator/HARDWARE.md` is credited here as the origin of the
hardware facts cited in `docs/timing-baseline.md` and `docs/controller-port.md`
— it comes from a decap and the chip vendor's handbook, and is the only
non-emulator source in the project.

An earlier revision of this file claimed that the clone's `dump/` held Sega's
SMPC ROM and that `reference_docs/` held the Saturn Service Manual. Neither is
true of what the Codeberg repository actually serves: `dump/` is not there at
all, and `reference_docs/` holds the Hitachi handbook, one datasheet and a die
trace. The error was repeated from `reference/README.md`, which had claimed the
two authoritative documents shipped in the clone. Both are corrected in place;
they are recorded here because a provenance file that states something false is
worse than one that says nothing.

## Sega and Hitachi material

Not redistributed, at all, in any form. This includes the SMPC ROM
(`315-5744`), the SMPC manual `ST-169-R1-072694`, and pages of the Saturn
Service Manual. They remain the property of their owners. `docs/` cites them
by number and page so a reader can find them; the repository contains none of
them.

## Trademarks

Sega Saturn, Sega and Mega Drive are trademarks of their respective owners.
SMPC, HD404920 and HMCS400 are Hitachi part names. This project is an
independent, clean-room-adjacent reimplementation for study; it is not
affiliated with, endorsed by, or sponsored by any of the above.
