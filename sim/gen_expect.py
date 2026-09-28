#!/usr/bin/env python3
"""Generate the hardware-derived expectation files for sim/differ.sh.

Two oracles, deliberately kept apart:

  * this file -- what the register dump *should* contain, derived from the
    BlueRetro controller-port driver (`reference/bluRetro/main/wired/sega_io.c`,
    which drives a real Saturn pad port from an ESP32 and is validated against
    real consoles) plus the SMPC's own report framing.  This column is
    authoritative: differ.sh fails on it.

  * the Beetle run, recorded with --record-beetle.  Beetle is a reference, not
    an oracle -- it has known, documented disagreements with the hardware
    reference (mouse report length, multi-tap count nybble, OREG pre-fill) --
    so its column is reported as INFO and never fails a scenario.

Any byte this file cannot derive is written as `--` and skipped by the
differ.  That is deliberate: a guess recorded as a fact is worse than a hole,
and the header of each .expect file says which bytes are holes and why.

Every derived field carries the source line it came from, and those citations
are *verified*: verify_citations() re-reads sega_io.c and fails the build if a
cited constant is no longer where it was cited from, so the derivations
cannot silently rot when either tree moves.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BLUERETRO = os.path.join(ROOT, "reference", "bluRetro", "main", "wired", "sega_io.c")
BEETLE_SMP = os.path.join(ROOT, "reference", "beetle", "smpc.c")
TRACE_DIR = os.path.join(HERE, "traces")
TB = os.path.join(ROOT, "build", "tb")

# --------------------------------------------------------------------------
# Citations.  (file, line, pattern) -- all must hold or we refuse to generate.
# --------------------------------------------------------------------------

CITATIONS = [
    # ID taxonomy
    (BLUERETRO, 67, "#define ID1_MOUSE 0x3"),
    (BLUERETRO, 70, "#define ID1_SATURN_PAD 0xB"),
    (BLUERETRO, 78, "#define ID2_SATURN_MULTITAP 0x4"),
    (BLUERETRO, 72, "#define ID1_NON_CONNECTION 0xF"),
    (BLUERETRO, 74, "#define ID2_SATURN_PAD 0x0"),
    (BLUERETRO, 75, "#define ID2_SATURN_ANALOG_PAD 0x1"),
    (BLUERETRO, 79, "#define ID2_LEGACY 0xE"),
    # ID0 bytes, which are the terminator nybbles
    (BLUERETRO, 49, "#define ID0_MOUSE 0x0B"),
    (BLUERETRO, 52, "#define ID0_SATURN_THREEWIRE_HANDSHAKE 0x11"),
    # digital pad packet: ID2|size, two button bytes, terminator
    (BLUERETRO, 280, "buffer[0] = (ID2_SATURN_PAD << 4) | 2;"),
    (BLUERETRO, 281, "buffer[1] = wired_adapter.data[src_port].output[0] | wired_adapter.data[src_port].output_mask[0];"),
    (BLUERETRO, 282, "buffer[2] = wired_adapter.data[src_port].output[1] | wired_adapter.data[src_port].output_mask[1];"),
    (BLUERETRO, 283, "buffer[3] = ID0_SATURN_THREEWIRE_HANDSHAKE >> 4;"),
    (BLUERETRO, 285, "twh_tx(port, buffer, 4, 0);"),
    # analog pad packet: ID2|size, buttons, AX, AY, AR, AL
    (BLUERETRO, 292, "buffer[0] = (ID2_SATURN_ANALOG_PAD << 4) | 6;"),
    (BLUERETRO, 293, "buffer[1] = wired_adapter.data[src_port].output[0] | wired_adapter.data[src_port].output_mask[0];"),
    (BLUERETRO, 303, "twh_tx(port, buffer, 8, 0);"),
    (BLUERETRO, 301, "buffer[7] = ID0_SATURN_THREEWIRE_HANDSHAKE >> 4;"),
    # mouse packet
    (BLUERETRO, 310, "buffer[0] = 0xFF;"),
    (BLUERETRO, 321, "twh_tx(port, buffer, 5, 14);"),
    # multi-tap: header, count in the HIGH nybble, per-slot ID2|size
    (BLUERETRO, 342, "*data++ = (ID2_SATURN_MULTITAP << 4) | 1;"),
    (BLUERETRO, 343, "*data++ = nb_port << 4;"),
    (BLUERETRO, 345, "for (uint32_t i = 0, j = first_port; i < nb_port; i++, j++) {"),
    (BLUERETRO, 349, "*data++ = (ID2_SATURN_PAD << 4) | 2;"),
    (BLUERETRO, 350, "*data++ = wired_adapter.data[j].output[0] | wired_adapter.data[j].output_mask[0];"),
    (BLUERETRO, 351, "*data++ = wired_adapter.data[j].output[1] | wired_adapter.data[j].output_mask[1];"),
    (BLUERETRO, 379, "*data++ = (ID2_LEGACY << 4) | 3;"),
    (BLUERETRO, 387, "*data++ = ID0_SATURN_THREEWIRE_HANDSHAKE >> 4;"),
    # SMPC report framing (emulator-derived -- Beetle is the only source)
    (BEETLE_SMP, 1540, "JRS.IDTap = 0xF1;"),
    (BEETLE_SMP, 1543, "JRS.TapCount = (JRS.IDTap & 0xF);"),
    (BEETLE_SMP, 1559, "JRS.ReadCount = ((JRS.ID2 & 0xF0) == 0xF0) ? 0 : (JRS.ID2 & 0xF);"),
    (BEETLE_SMP, 1526, "JRS.ID2 = 0xE3;"),
]

# --------------------------------------------------------------------------
# Scenario constants, mirrored from sim/tb.c.
# --------------------------------------------------------------------------

# setup_common()'s set_rtc(): November 21st 2000, 15:04:05, Tuesday.
TM = dict(year=2000, mon=11, mday=21, hour=15, minute=4, second=5, wday=2)
LANG = 1                      # set_rtc(..., 1) -- English; also smem[3]
CLOCK_DIVISOR_26M = 65        # smpc_reset() leaves the 26 MHz divisor in


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def rtc_bytes(tm):
    """SMPC_SetRTC()'s packing, reference/beetle/smpc.c:457-464."""
    return [
        bcd(tm["year"] // 100), bcd(tm["year"] % 100),
        (tm["wday"] << 4) | tm["mon"],
        bcd(tm["mday"]), bcd(tm["hour"]), bcd(tm["minute"]), bcd(tm["second"]),
    ]


def scale_axis(v):
    """Unsigned 16-bit travel -> 0..255, dead centre snapped.

    The 16-bit value is *unsigned*: 0x0000 and 0xFFFF are opposite ends of
    travel, so a signed cast would put one end of every stick at zero.
    """
    if 32768 - 128 <= v < 32768:
        v = 32768
    return (v * 255 + 32767) // 65535


def wire_buttons(dbuttons):
    """The two button bytes as they appear on R/L/D/U.

    BlueRetro sends `output[]` verbatim (sega_io.c:281-282); the wire reads
    1 when a button is released and 0 when it is pressed, so each 4-bit
    nybble is inverted on the way out.  Bit set in `dbuttons` == pressed.
    """
    lo = (~dbuttons) & 0xF
    b0 = (lo << 4) | ((~dbuttons >> 4) & 0xF)
    b1 = ((~dbuttons >> 8) & 0xF) << 4 | ((~dbuttons >> 12) & 0xF)
    return b0 & 0xFF, b1 & 0xFF


def shoulder_digital_bits(dbuttons, left, right):
    """The trigger axes also drive digital L (bit 11) and R (bit 15).

    Hysteresis from a measurement: engage at >= 0x8E, release at <= 0x55.
    A pad with its triggers held down therefore reports the right trigger a
    second time, inside the button bytes -- that is where `b1 = 0x07` in the
    all-buttons-down scenario comes from.
    """
    for w, axis in ((0, left), (1, right)):
        mask = 0x0800 << (w << 2)
        if axis <= 0x55:
            dbuttons &= ~mask
        elif axis >= 0x8E:
            dbuttons |= mask
    return dbuttons & 0xFFFF


def status_block(area, rtc, smem, last_cmd, divisor=CLOCK_DIVISOR_26M,
                 slave=False, sound=False, cd=True, rtc_valid=True):
    """OREG0..15 as written before the peripheral half overwrites the head.

    Field-by-field, from core/src/smpc.c:1297-1309 (which documents the
    disagreement with MAME/Kronos about the undefined tail):
      [0]     RTC battery valid (bit 7) | reset-button NMI disabled (bit 6)
      [1..7]  the RTC, packed as the status report wants it
      [8]     cartridge code -- not modelled, always 0
      [9]     area code
      [0xA]   0x24 | 28 MHz | slave SH-2 | master NMI pending |
              system reset state | sound CPU on
      [0xB]   CD on | 0x02
      [0xC..F] the four NVRAM bytes
      [10..1E] never written; 0, not 0xFF (see the note in smpc.c)
      [1F]    the command that produced the report
    """
    b = [0x00] * 32
    b[0] = (0x80 if rtc_valid else 0) | 0x40          # reset-NMI off by default
    b[1:8] = rtc
    b[8] = 0x00
    b[9] = area & 0x0F
    b[0xA] = (0x24
              | (0x40 if divisor == 61 else 0)
              | (0x10 if slave else 0)
              | 0x08 | 0x02
              | (0x01 if sound else 0))
    b[0xB] = (0x40 if cd else 0) | 0x02
    b[0xC:0x10] = smem
    b[0x1F] = last_cmd
    return b


def base_smem():
    """set_rtc()'s side effect: the language byte lands in NVRAM[3]."""
    return [0x00, 0x00, 0x00, LANG]


def fmt(words):
    return " ".join(w if isinstance(w, str) else "%02X" % w for w in words)


def sr_after(intback, peripheral):
    """SR as both models leave it after an INTBACK.

      no INTBACK            -> 0x00
      status report only    -> 0x0F  (bit 7 clear, port-mode nybbles)
      peripheral data read  -> 0xC0  (bit 7 set, plus PDL)
    """
    if not intback:
        return 0x00
    return 0xC0 if peripheral else 0x0F


# --------------------------------------------------------------------------
# Per-scenario expectations
# --------------------------------------------------------------------------

REPORT_NONE = None


def pad_head(buttons, l=0, r=0):
    """The first four bytes of a directly-connected Saturn Control Pad.

    Report header, packet id, two button bytes.  No terminator, so the
    result can be spliced in front of another port's report -- a multi-tap
    on port 1 leaves port 0's pad as the head of the whole thing.
    """
    dbuttons = shoulder_digital_bits(buttons & 0xFFFF, scale_axis(l), scale_axis(r))
    b0, b1 = wire_buttons(dbuttons)
    # F1 = no multi-tap, one device (the report header; Beetle smpc.c:1540)
    # 02 = (ID2_SATURN_PAD << 4) | 2        sega_io.c:74 + sega_io.c:280
    # ..  = the two button bytes             sega_io.c:281-282
    return [0xF1, 0x02, b0, b1]


def report_one_pad(buttons=0x0000, l=0, r=0):
    """One Saturn Control Pad on port 0, nothing on port 1."""
    # F0 = port 1: ID1_NON_CONNECTION (sega_io.c:72), then 0
    return pad_head(buttons, l, r) + [0xF0]


def report_analog(buttons, x, y, l, r):
    """A 3D Control Pad in analog mode: buttons, then AX, AY, AR, AL."""
    dbuttons = buttons & 0x0FFF           # bit 12 is the mode flag, not a button
    dbuttons = shoulder_digital_bits(dbuttons, scale_axis(l), scale_axis(r))
    b0, b1 = wire_buttons(dbuttons)
    # 16 = (ID2_SATURN_ANALOG_PAD << 4) | 6   sega_io.c:75 + sega_io.c:292
    return [0xF1, 0x16, b0, b1,
            scale_axis(x), scale_axis(y), scale_axis(l), scale_axis(r),
            0xF0]


def report_mouse():
    """The head of a mouse report, plus the holes its payload opens up."""
    # F1  = report header, no multi-tap, one device   beetle smpc.c:1540
    # 1..15 hole.  The report is ID2 + payload + port 1, and neither the
    #   payload length nor ID2 is settled by the sources in this tree:
    #   Beetle force-substitutes ID2 = 0xE3 (smpc.c:1525-1526) and then
    #   reads three data bytes, while BlueRetro's standalone mouse puts
    #   0xFF on the wire (sega_io.c:310) and sends four (sega_io.c:308-321);
    #   a mouse behind a tap uses 0xE3 (sega_io.c:379).  The two readings
    #   differ by a byte, which slides the status tail behind this by one.
    # 16..30 0.  Status reports stop at [15] and the peripheral report is
    #   nowhere near long enough to reach [16], so nothing writes them.
    # 10  = OREG[31], the last command
    return [0xF1] + ["--"] * 15 + [0x00] * 15 + [0x10]


def report_multitap(subs):
    """Three sub-slots behind a multi-tap on port 0.

    Byte 0 is the report header: upper nybble = which device sits on the
    port, lower = how many are behind it.  For a port with nothing attached
    that is 0xF1, which the single-pad scenarios confirm.  For a multi-tap
    the count comes from BlueRetro's `nb_port << 4` (sega_io.c:343), i.e.
    the HIGH nybble -- and Beetle reads the LOW one (smpc.c:1533), which is
    a documented disagreement in docs/controller-port.md.  The upper nybble
    of the header for a tap is not stated anywhere, so byte 0 is a hole and
    the two candidate values (our deduction vs Beetle's 0x16) are recorded
    rather than one of them being silently picked.

    Each sub-slot is `(ID2_SATURN_ANALOG_PAD << 4) | 6` (sega_io.c:356)
    followed by the six data bytes.  What follows the last slot is wherever
    the two readings of the count disagree, so it is a hole too.
    """
    out = ["--"]
    for buttons, x, y, l, r in subs:
        dbuttons = buttons & 0x0FFF
        dbuttons = shoulder_digital_bits(dbuttons, scale_axis(l), scale_axis(r))
        b0, b1 = wire_buttons(dbuttons)
        out += [0x16, b0, b1,
                scale_axis(x), scale_axis(y), scale_axis(l), scale_axis(r)]
    out += ["--"] * 9
    return out


def build_expectations():
    """{scenario: (header_comment_lines, [ [oreg tokens], [sr], ... ])}"""
    rtc_tm = rtc_bytes(TM)
    out = {}

    # ---- status_only: status report, no peripheral half -------------------
    oreg = status_block(0x5, rtc_tm, base_smem(), 0x10)
    out["status_only"] = ([
        "one INTBACK asking for the status report only (IREG1 = 0x00), so the",
        "peripheral half never runs and OREG is the status block verbatim.",
        "status block: core/src/smpc.c:1297-1309",
        "RTC:         reference/beetle/smpc.c:457-464, from setup_common()'s tm",
        "NVRAM[3]:    core/src/smpc.c:485 -- set_rtc() stores the language byte",
    ], [[oreg], [sr_after(True, False)]])

    # ---- intback_one_pad --------------------------------------------------
    rep = report_one_pad()
    oreg = status_block(0x5, rtc_tm, base_smem(), 0x10)
    oreg[:len(rep)] = rep
    out["intback_one_pad"] = ([
        "one Saturn Control Pad on port 0, nothing on port 1.",
        "  F1  report header, no multi-tap, one device  beetle smpc.c:1540",
        "  02  (ID2_SATURN_PAD << 4) | 2               sega_io.c:74,280",
        "  FF  button bytes, all released              sega_io.c:281-282",
        "  F0  port 1 empty, ID1_NON_CONNECTION        sega_io.c:72",
        "the report overwrites the head of the status block; bytes 5..15 are",
        "whatever the status block put there.",
    ], [[oreg], [sr_after(True, True)]])

    # ---- intback_analog ---------------------------------------------------
    rep = report_analog(0x1FFF, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF)
    oreg = status_block(0x5, rtc_tm, base_smem(), 0x10)
    oreg[:len(rep)] = rep
    out["intback_analog"] = ([
        "3D Control Pad in analog mode, every button down, sticks and",
        "triggers at opposite ends of travel.",
        "  F1  report header                          beetle smpc.c:1540",
        "  16  (ID2_SATURN_ANALOG_PAD << 4) | 6       sega_io.c:75,292",
        "  00  buttons 0..7, all pressed, wire low",
        "  07  buttons 8..11 all pressed, plus digital R at bit 15 -- the",
        "      right trigger's axis also drives a digital button, so a pad",
        "      with its triggers held reports it twice",
        "  00 FF FF FF  AX, AY, AR, AL -- unsigned travel, 0x8000 is centre",
        "  F0  port 1 empty                           sega_io.c:72",
    ], [[oreg], [sr_after(True, True)]])

    # ---- intback_mouse ----------------------------------------------------
    oreg = report_mouse()
    out["intback_mouse"] = ([
        "SEGA Mouse on port 0.  Only two of the thirty-two bytes survive the",
        "payload-length disagreement; see report_mouse().",
        "  F1  report header                          beetle smpc.c:1540",
        "  --  ID2: Beetle force-substitutes 0xE3     beetle smpc.c:1525-1526",
        "               BlueRetro's standalone mouse sends 0xFF sega_io.c:310",
        "               and only a mouse behind a tap uses 0xE3 sega_io.c:379",
        "  --  payload: 4 bytes on hardware (sega_io.c:308-321) vs 3 in",
        "               Beetle -- recorded in docs/controller-port.md; the",
        "               two lengths slide the status tail by one byte",
        "  00  OREG[16..30]: neither report nor status block reaches them",
        "  10  OREG[31] does not move: it is the last command",
    ], [[oreg], [sr_after(True, True)]])

    # ---- intback_multitap -------------------------------------------------
    subs = [
        (0x1001, 0x8000, 0x8000, 0x0000, 0x0000),
        (0x1002, 0x8000, 0x8000, 0x0000, 0x0000),
        (0x1003, 0x8000, 0x8000, 0x0000, 0x0000),
    ]
    rep = report_multitap(subs)
    oreg = status_block(0x5, rtc_tm, base_smem(), 0x10)
    for i, w in enumerate(rep):
        oreg[i] = w
    out["intback_multitap"] = ([
        "three analog pads behind a multi-tap on port 0.",
        "  --  report header.  Its lower nybble is the sub-slot count, and",
        "      BlueRetro puts that count in the HIGH nybble of its count byte",
        "      (sega_io.c:343) while Beetle reads the LOW one (beetle smpc.c:1533),",
        "      so the two readings disagree by a factor of sixteen.  The upper",
        "      nybble of the header for a tap is stated nowhere either.  Both",
        "      candidates are recorded rather than one being picked, and the",
        "      gap it opens in the tail is left open too.",
        "  16  (ID2_SATURN_ANALOG_PAD << 4) | 6, once per sub-slot",
        "                                                  sega_io.c:356",
        "  ..  b0, b1, AX, AY, AR, AL per slot, derived from the scenario",
        "  --  [22..30]: the tail's start depends on the count above",
        "  10  OREG[31] is the last command and does not move",
    ], [[oreg], [sr_after(True, True)]])

    # ---- intback_digital_buttons -----------------------------------------
    rep = report_one_pad(0x0A35, 0x8000, 0x8000)
    oreg = status_block(0x5, rtc_tm, base_smem(), 0x10)
    oreg[:len(rep)] = rep
    out["intback_digital_buttons"] = ([
        "one Saturn Control Pad on port 0 with an asymmetric button pattern.",
        "  F1  report header, no multi-tap, one device  beetle smpc.c:1540",
        "  02  (ID2_SATURN_PAD << 4) | 2               sega_io.c:74,280",
        "  AC  buttons 0..7 of 0x0A35, inverted on the way out   sega_io.c:281",
        "  5F  buttons 8..15 of 0x0A35                          sega_io.c:282",
        "  F0  port 1 empty, ID1_NON_CONNECTION        sega_io.c:72",
        "the pattern is deliberately asymmetric: the all-released and",
        "all-pressed patterns read the same whether bits 0..3 land in the high",
        "nybble or the low one, so neither can tell the two packings apart.",
        "all four axes sit at 0x8000, which scale_axis maps to 128 -- inside",
        "the 0x56..0x8D hysteresis gap, so digital L and R do not move.",
        "the report overwrites the head of the status block; bytes 5..15 are",
        "whatever the status block put there.",
    ], [[oreg], [sr_after(True, True)]])

    # ---- intback_multitap_p1 ---------------------------------------------
    # Port 0 is a plain control pad; the multi-tap sits on port 1 instead,
    # which is the other half of Beetle's MapPorts() cursor: port 0 is not
    # swallowed by the tap, so it keeps its own four-byte report and the
    # tap's six sub-slots follow it.
    subs = [(0x0331, 0, 0), (0x0662, 0, 0), (0x0993, 0, 0)]
    rep = list(pad_head(0x0A05, 0, 0)) + ["--"]
    for buttons, l, r in subs:
        dbuttons = shoulder_digital_bits(buttons & 0xFFFF, scale_axis(l),
                                         scale_axis(r))
        b0, b1 = wire_buttons(dbuttons)
        rep += [0x02, b0, b1]
    rep += ["--"] * 3
    oreg = status_block(0x5, rtc_tm, base_smem(), 0x10)
    for i, w in enumerate(rep):
        oreg[i] = w
    out["intback_multitap_p1"] = ([
        "a direct control pad on port 0 and a multi-tap on port 1.",
        "  F1 02 AF DF  port 0: report header, packet id, buttons 0x0A05",
        "      with both triggers at rest -- scale_axis(0) is 0, inside the",
        "      release threshold, so digital L (bit 11) and digital R (bit 15)",
        "      are cleared first        sega_io.c:74,280-282",
        "  --  the tap's report header.  It is not stated by either source:",
        "      BlueRetro sends the adapter id and the count as two bytes and",
        "      puts the count in the HIGH nybble (sega_io.c:342-343) while",
        "      Beetle reads the LOW one (beetle smpc.c:1533), and the upper",
        "      nybble of the header is stated nowhere at all.",
        "  02  (ID2_SATURN_PAD << 4) | 2, once per sub-slot   sega_io.c:349",
        "  ..  b0, b1 for the three pads behind the tap: ports 1..3 hold",
        "      0x0331, 0x0662 and 0x0993, each with its triggers at rest",
        "                                            sega_io.c:350-351",
        "  --  [14..16]: the three sub-slots with nothing in them.  How many",
        "      slots exist is exactly what the sources disagree on -- ours and",
        "      Beetle pad the stream to six and an empty one reads as 0xFF,",
        "      while BlueRetro stops after nb_port sub-slots (sega_io.c:344)",
        "      and closes with a single terminator nybble (sega_io.c:388).",
        "  00  [17..30]: unwritten under both readings.  The status block",
        "      stops at [15] and neither version of the report reaches here.",
        "  10  OREG[31] is the last command and does not move",
    ], [[oreg], [sr_after(True, True)]])

    # ---- intback_rtc_tick -------------------------------------------------
    # 330 NTSC frames is 5.5 s of video at 60 Hz -- a fact about the console,
    # not about this model.  The RTC is 32 768 cycles of a 32.768 kHz crystal
    # (reference/smpc-emulator/HARDWARE.md), i.e. one second, so five whole
    # seconds have passed and the clock reads 05 + 5.
    rtc_5s = dict(TM, second=TM["second"] + 5)
    oreg = status_block(0x5, rtc_bytes(rtc_5s), base_smem(), 0x10)
    out["intback_rtc_tick"] = ([
        "the RTC advancing on its own, which no other scenario could see.",
        "  330 NTSC frames = 5.5 s of video at 60 Hz, so floor(5.5) = 5 whole",
        "      RTC seconds pass and [7], the seconds byte, goes 05 -> 10.",
        "      The BCD carry is the point: 09 + 1 must be 0x10, not 0x0A.",
        "  0x8000 milliseconds of running time is the only way to see it.  The",
        "      model drains the RTC in do_vblank_housekeeping(), so it only",
        "      ticks on a vblank edge, and every scenario that reports the",
        "      status block advances under a second.",
        "the model agrees with the derivation, but not by a wide margin and",
        "not for the reason it should.  The harness emits 28 MHz frames while",
        "the SMPC sits in its power-on 26 MHz mode -- nothing has issued",
        "CKCHG352 -- so a frame is worth 1.0654 model seconds and 330 frames",
        "is 5.858, not 5.5.  The floor is still 5, but only 0.142 s clear of",
        "the next boundary.  See docs/timing-baseline.md.",
    ], [[oreg], [sr_after(True, False)]])

    # ---- settime_smem -----------------------------------------------------
    smem_cmd = [0x08, 0x03, 0x10, 0x11]
    rtc_cmd = [0x08, 0x03, 0x10, 0x11, 0x12, 0x13, 0x14]
    oreg = status_block(0xC, rtc_cmd, smem_cmd, 0x10, divisor=CLOCK_DIVISOR_26M)
    out["settime_smem"] = ([
        "SETTIME then SETSMEM then a status-only INTBACK, so the dump shows",
        "both commands round-tripped through the RTC and the NVRAM.",
        "  [1..7]   SETTIME copies IREG0..6 into the RTC",
        "  [9]      area code 0xC (EU PAL)",
        "  [C..F]   SETSMEM copies IREG0..3 into the NVRAM",
        "note: this scenario's SF dump differs between the two models only in",
        "the open-bus bits above bit 0 (last COMREG byte vs last IREG0 byte),",
        "which is bus wiring rather than SMPC behaviour -- see differ.sh.",
    ], [[oreg], [sr_after(True, False)]])

    # ---- command_matrix ---------------------------------------------------
    cmds = [0x00, 0x02, 0x03, 0x06, 0x07, 0x08, 0x09, 0x18, 0x19, 0x1A]
    groups = []
    for c in cmds:
        oreg = [0x00] * 32
        oreg[0x1F] = c
        groups.append(oreg)
    out["command_matrix"] = ([
        "the simple commands, none of which produce a report, so OREG is",
        "zeros apart from [31] carrying the command just issued.  SR stays",
        "0 -- no INTBACK ran.",
        "OREG[31] = the command: core/src/smpc.c (ST_COMMAND_LATENCY)",
    ], [groups, [0x00] * len(cmds)])

    # ---- direct_mode ------------------------------------------------------
    # IREG0 = 0x00, so the status half is skipped entirely: OREG keeps its
    # power-on zeros where a status report would have been.
    rep = report_one_pad()
    oreg = [0x00] * 32
    oreg[:len(rep)] = rep
    oreg[0x1F] = 0x10
    out["direct_mode"] = ([
        "the SH-2 pokes PDR/DDR/IOSEL directly, then an INTBACK with",
        "IREG0 = 0x00.  The low nybble of IREG0 selects the status half, so",
        "zero means it is skipped and OREG is the report over power-on",
        "zeros rather than over the status block.",
        "  F1 02 FF FF F0  exactly as intback_one_pad, port 0 digital,",
        "                   port 1 empty",
    ], [[oreg], [sr_after(True, True)]])

    # ---- sysres_ckchg -----------------------------------------------------
    oreg = [0x00] * 32
    oreg[0x1F] = 0x0D
    out["sysres_ckchg"] = ([
        "SYSRES then CKCHG352.  No INTBACK, so SR is 0 and OREG is zeros",
        "except [31], which keeps reporting SYSRES (0x0D) -- CKCHG was",
        "issued in the window between SYSRES and the frame boundary, and",
        "the reset takes the pending command with it.",
    ], [[oreg], [0x00]])

    return out


# --------------------------------------------------------------------------

def verify_citations():
    problems = []
    for path, line, pattern in CITATIONS:
        if not os.path.exists(path):
            problems.append("%s:%d missing file %s" % (path, line, path))
            continue
        with open(path, "r", errors="replace") as f:
            lines = f.read().splitlines()
        if line > len(lines) or lines[line - 1].strip() != pattern.strip():
            got = lines[line - 1].strip() if line <= len(lines) else "<eof>"
            problems.append(
                "%s:%d\n    cited: %s\n    found: %s"
                % (os.path.relpath(path, ROOT), line, pattern, got))
    return problems


def write_expect(name, comment, groups):
    path = os.path.join(TRACE_DIR, name + ".expect")
    with open(path, "w") as f:
        f.write("# %s -- hardware-derived expectation.  Generated by\n"
                "# sim/gen_expect.py; do not edit by hand.\n"
                "#\n" % name)
        for line in comment:
            f.write("# %s\n" % line)
        f.write("#\n"
                "# `--` means not derived and not compared.\n")
        for gi, group in enumerate(groups):
            if gi:
                f.write("---\n")
            oreg, sr = group
            f.write("OREG %s\n" % fmt(oreg))
            f.write("SR   %02X\n" % sr)
    return path


def record_beetle(names):
    os.makedirs(TRACE_DIR, exist_ok=True)
    for name in names:
        r = subprocess.run([TB, "--trace", "beetle", name],
                           capture_output=True, text=True, timeout=300)
        if r.returncode != 0:
            sys.stderr.write("tb beetle %s failed:\n%s" % (name, r.stderr))
            return 1
        with open(os.path.join(TRACE_DIR, name + ".beetle"), "w") as f:
            f.write(r.stdout)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--record-beetle", action="store_true",
                    help="also run the Beetle backend and save its trace")
    ap.add_argument("--check", action="store_true",
                    help="verify the source citations and exit")
    ap.add_argument("scenarios", nargs="*")
    args = ap.parse_args()

    problems = verify_citations()
    if problems:
        sys.stderr.write("citation drift -- gen_expect.py needs updating:\n")
        for p in problems:
            sys.stderr.write("  %s\n" % p)
        return 1
    if args.check:
        print("citations ok (%d)" % len(CITATIONS))
        return 0

    os.makedirs(TRACE_DIR, exist_ok=True)
    all_exp = build_expectations()
    names = args.scenarios or sorted(all_exp)
    for name in names:
        if name not in all_exp:
            sys.stderr.write("unknown scenario: %s\n" % name)
            return 2
        comment, groups = all_exp[name]
        # normalise: groups is a list of either [oreg] or [oregs, srs]
        if len(groups) == 2 and isinstance(groups[0][0], list):
            oregs, srs = groups
            pairs = list(zip(oregs, srs))
        else:
            pairs = [(groups[0][0], groups[1][0])]
        path = write_expect(name, comment, pairs)
        print("wrote %s" % os.path.relpath(path, ROOT))

    if args.record_beetle:
        return record_beetle(names)
    return 0


if __name__ == "__main__":
    sys.exit(main())
