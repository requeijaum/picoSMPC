/*
 * tb.c -- stimulus driver shared by both backends, plus the scenarios.
 *
 * The virtual clock is the SH-2 master clock, in Beetle's units (master
 * clock ticks).  A frame is master_clock / (60/1.001) / 2 ticks; vblank
 * occupies the last ~4 ms of the VDP's vertical period.  We do not emulate
 * the VDP -- we only need enough of the vblank/vsync waveform to exercise
 * the SMPC's vblank-dependent INTBACK paths, so we model vb and vsync as
 * two rectangles with the same timing the VDP2 hook in Beetle sees.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "tb.h"
#include "trace.h"

const SmpcBackend *tb_backend;
void tb_set_backend(const SmpcBackend *be) { tb_backend = be; }

static int32_t g_ts;
static int32_t g_master_clock = 28636364;
static int     g_clock_mode = TB_CLOCK_NTSC_352;

static uint32_t g_irq_count;

int32_t tb_now(void) { return g_ts; }
uint32_t tb_irq_count(void) { return g_irq_count; }

/*
 * Bus decode, done once here rather than inside each backend.
 *
 * The SH-2's six address lines that reach the SMPC are A2..A7, so the SMPC
 * register index is (byte address & 0x7F) >> 1, and the top bit aliases
 * (0x40..0x7F mirrors 0x00..0x3F).  This is bus wiring, not SMPC behaviour,
 * so it belongs outside both models -- which also means the two backends
 * provably see the same register index.
 */
uint8_t tb_smpc_addr(uint8_t sh2_addr)
{
	return (uint8_t)(((sh2_addr & 0x7F) >> 1) & 0x3F);
}

int32_t tb_master_clock_hz(int clock_mode)
{
	switch (clock_mode) {
	case TB_CLOCK_PAL_352:
	case TB_CLOCK_PAL_320:  return 28437500;
	default:                 return 28636364;
	}
}

/*
 * The argument smpc_init() wants is NOT the SH-2 clock.  It is that clock
 * already multiplied by the clock divisor, which is what Mednafen's ss.c
 * passes: 1746818182 for NTSC, i.e. 28636364 * 61.  The divisor in
 *
 *     ClockRatio = (1 << 32) * 4000000 * CurrentClockDivisor / MasterClock
 *
 * then cancels, leaving 4e6 / 28.636364 MHz -- 0.1397 core clocks per host
 * tick, so sixty video frames come to one RTC second, as they must.
 *
 * Handing it the undivided clock instead leaves the divisor uncancelled: the
 * model ran at 8.52 core clocks per tick, which measured 61.2x too fast, and
 * no test could see it because both backends got the same wrong value.  See
 * docs/timing-baseline.md.
 *
 * 61 unconditionally, in every mode, because that is what the reference does.
 * Beetle keeps one MasterClock for the whole run and lets CurrentClockDivisor
 * move 65 <-> 61 across a CKCHG, which leaves 320 mode about 1% fast.  Matching
 * that is deliberate: the two columns are only worth comparing if the harness
 * reproduces the reference's mistake as well as its formula.
 */
#define TB_SMPC_MASTER_CLOCK_SCALE 61

int32_t tb_smpc_master_clock(int clock_mode)
{
	return tb_master_clock_hz(clock_mode) * TB_SMPC_MASTER_CLOCK_SCALE;
}

int32_t tb_line_cycles(int clock_mode)
{
	const int lines = (clock_mode == TB_CLOCK_PAL_352 || clock_mode == TB_CLOCK_PAL_320) ? 313 : 263;
	return (tb_master_clock_hz(clock_mode) / (clock_mode >= TB_CLOCK_PAL_352 ? 50 : 60)) / lines;
}

/* Advance the virtual clock, running the SMPC at every wakeup it requests.
 * Mirrors how the real machine behaves: SMPC_Update returns the next
 * timestamp it wants to be called at, and we honour it. */
void tb_advance_to(int32_t ts)
{
	int guard = 0;

	if (ts < g_ts) {
		fprintf(stderr, "tb: time went backwards (%d -> %d)\n", g_ts, ts);
		abort();
	}
	/*
	 * Step only to the wakeups the model asks for, never to a fixed grid.
	 * Both models therefore get exactly the same stimulus, and the trace
	 * timestamps are the real event times rather than the harness's
	 * polling boundaries.  trace_clock() is refreshed before every call
	 * because a model can emit several events from a single update.
	 */
	while (g_ts < ts) {
		int32_t next;

		if (tb_backend->poll_system)
			tb_backend->poll_system();
		trace_clock(g_ts);
		next = tb_backend->update(g_ts);
		if (tb_backend->earliest_event) {
			const int32_t hint = tb_backend->earliest_event();
			if (hint < next)
				next = hint;
		}
		if (next <= g_ts)
			next = g_ts + 1;
		if (next > ts)
			next = ts;
		g_ts = next;
		if (++guard > 40000000) {
			fprintf(stderr, "tb: stuck advancing to %d (at %d)\n", ts, g_ts);
			abort();
		}
	}
	if (tb_backend->poll_system)
		tb_backend->poll_system();
	trace_clock(g_ts);
	tb_backend->update(g_ts);
}

void tb_advance(int32_t delta) { tb_advance_to(g_ts + delta); }

/* Roughly one second of master clock: an SF-wait that has not retired by
 * then is a genuine hang, not a slow model. */
#define TB_SF_DEADLINE 28636364

/* ---- peripheral input buffers ---- */
void tb_pad_clear(uint8_t *buf)
{
	memset(buf, 0, 16);
}

void tb_pad_set_buttons(uint8_t *buf, uint16_t buttons)
{
	buf[0] = (uint8_t)(buttons & 0xFF);
	buf[1] = (uint8_t)(buttons >> 8);
}

void tb_write(int32_t ts, uint8_t sh2_addr, uint8_t value)
{
	tb_backend->write(ts, tb_smpc_addr(sh2_addr), value);
}

/*
 * Count System Manager interrupts.  Routed through the shared trace so both
 * backends are counted the same way.
 */
void tb_note_irq(void)
{
	g_irq_count++;
}

uint8_t tb_read(int32_t ts, uint8_t sh2_addr)
{
	return tb_backend->read(ts, tb_smpc_addr(sh2_addr));
}

static void dump_regs(void)
{
	uint8_t oreg[32];
	for (int i = 0; i < 32; i++)
		oreg[i] = tb_read(g_ts, (uint8_t)(TB_OREG0 + i * 2));
	trace_reg_array("OREG", oreg, 32);
	trace_reg_byte("SR", tb_read(g_ts, TB_SR));
	trace_reg_byte("SF", tb_read(g_ts, TB_SF));
}

/* Issue a command the way the Saturn BIOS does: write SF=1, then COMREG.
 * IREGs go down first. */
static void issue(uint8_t cmd, const uint8_t *iregs, int nregs)
{
	for (int i = 0; i < nregs; i++)
		tb_write(g_ts, (uint8_t)(TB_IREG0 + i * 2), iregs[i]);
	tb_write(g_ts, TB_SF, 0x01);
	tb_write(g_ts, TB_COMREG, cmd);
}

/* Poll SF until the command retires, mirroring the BIOS's SF-wait loop. */
static void wait_sf(int32_t max_master_clocks)
{
	int32_t deadline = g_ts + max_master_clocks;
	while (tb_read(g_ts, TB_SF) & 1) {
		if (g_ts > deadline) {
			trace_mark("SF-POLL-TIMEOUT");
			return;
		}
		tb_advance(64);
	}
}

/* ================================================================== */
/* Scenarios                                                          */
/* ================================================================== */

/* One 16-byte input buffer per virtual port; ports 0..5 feed the six
 * possible peripheral slots (port 0, port 1, and four multi-tap subs). */
static uint8_t g_ports[6][16];
static uint8_t g_misc[8];

/*
 * Issue an INTBACK and service it the way the Saturn BIOS does.
 *
 * The BIOS's System Manager interrupt handler copies OREG out, then writes
 * IREG0 with the continue bit *toggled* to ask for the next 32 nybbles, and
 * repeats until SR's NPE bit says no more data is left.  Each continue is
 * tied to an interrupt, because that is the only moment the SMPC is actually
 * waiting for one.  A harness that just polls SF will hang, and both models
 * will hang identically -- which is a false pass, so the handshake has to be
 * modelled rather than waited out.
 */
static void intback_exchange(uint8_t cmd, const uint8_t *iregs, int nregs)
{
	const int32_t deadline = g_ts + TB_SF_DEADLINE;
	uint32_t seen = tb_irq_count();
	uint8_t cont = 0x80;	/* the SMPC's first expected continue is a 1 */

	issue(cmd, iregs, nregs);

	while (tb_read(g_ts, TB_SF) & 1) {
		if (g_ts > deadline) {
			trace_mark("SF-POLL-TIMEOUT");
			return;
		}
		tb_advance(64);
		if (tb_irq_count() != seen) {
			seen = tb_irq_count();
			/* Handler: latch the data, then ask for the next block.
			 * The bit is *sent* before it is toggled: the SMPC's first
			 * expected continue is a 1, so sending a toggled-from-zero
			 * value would look like a break. */
			tb_write(g_ts, TB_IREG0, cont);
			cont ^= 0x80;
		}
	}
}

static void setup_common(uint8_t area, int clock_mode, bool digital_pad)
{
	g_ts = 0;
	trace_reset();
	g_irq_count = 0;
	g_master_clock = tb_master_clock_hz(clock_mode);
	g_clock_mode = clock_mode;

	memset(g_ports, 0, sizeof g_ports);
	memset(g_misc, 0, sizeof g_misc);

	/*
	 * Neutral pad state: no buttons pressed, both thumbsticks centred.
	 *
	 * The device models keep buttons active *high* (bit set = pressed) and
	 * invert them onto the wire, so a released pad is 0 and the report comes
	 * out as 0xFF.  The thumbsticks are 16-bit signed with 0x8000 at centre.
	 */
	for (int i = 0; i < 6; i++) {
		tb_pad_set_buttons(&g_ports[i][TB_PAD_BTN], 0x0000);
		tb_pad_set_buttons(&g_ports[i][TB_PAD_THUMBX], 0x8000);
		tb_pad_set_buttons(&g_ports[i][TB_PAD_THUMBY], 0x8000);
	}
	(void)digital_pad;

	tb_backend->init(area, tb_smpc_master_clock(clock_mode), false);
	for (unsigned i = 0; i < 6; i++)
		tb_backend->set_input(i, "none", g_ports[i]);
	tb_backend->set_input(12, "misc", &g_misc[0]);
	tb_backend->reset(true);
	tb_backend->start_frame();

	{
		struct tm t = { 0 };
		t.tm_year = 100;	/* 2000 */
		t.tm_mon = 10;		/* November */
		t.tm_mday = 21;
		t.tm_hour = 15;
		t.tm_min = 4;
		t.tm_sec = 5;
		t.tm_wday = 2;
		t.tm_yday = 325;
		t.tm_isdst = 0;
		tb_backend->set_rtc(&t, 1 /* English */);
	}
}

/*
 * Drive one video frame's vblank and vsync.
 *
 * The VDP is not modelled; only the two edges the SMPC actually takes an
 * interrupt from are.  NTSC has 263 lines, and vblank occupies roughly the
 * last 32, of which the first 24 are vblank IN -> OUT.  The order matters:
 * the SMPC latches "vblank started" on the rising edge and consumes it on
 * its own next wakeup, so the edges have to arrive in VDP order.
 */
static void run_frame(void)
{
	const int32_t line = tb_line_cycles(g_clock_mode);
	const int32_t htotal = 263 * line;
	const int32_t vblank_start = htotal - 0x20 * line;
	const int32_t vblank_end = htotal - 0x08 * line;
	const int32_t frame_start = g_ts;

	tb_advance_to(frame_start + vblank_start);
	tb_backend->set_vbvs(g_ts, true, false);		/* VBlank IN  */
	tb_advance_to(frame_start + vblank_end);
	tb_backend->set_vbvs(g_ts, false, true);		/* VBlank OUT */
	tb_advance_to(frame_start + htotal);
	tb_backend->set_vbvs(g_ts, false, false);
}

/* ---- scenario 1: plain status report, no peripherals ---- */
static void sc_status_only(void)
{
	uint8_t iregs[3] = { 0x01, 0x00, 0xF0 };
	trace_mark("status_only (no pads, OPE=0)");
	setup_common(0x5 /* NA */, TB_CLOCK_NTSC_352, true);
	intback_exchange(0x10, iregs, 3);
	tb_advance(200000);
	dump_regs();
}

/* ---- scenario 2: status + peripheral report, one digital pad ---- */
static void sc_intback_one_pad(void)
{
	uint8_t iregs[3] = { 0x01, 0x08, 0xF0 };
	trace_mark("intback_one_pad (3dpad digital, no buttons)");
	setup_common(0x5, TB_CLOCK_NTSC_352, true);
	tb_backend->set_input(0, "3dpad", g_ports[0]);
	tb_backend->set_input(1, "none", g_ports[1]);
	tb_backend->reset(false);
	intback_exchange(0x10, iregs, 3);
	tb_advance(200000);
	dump_regs();
}

/* ---- scenario 3: all buttons pressed, analog mode ---- */
static void sc_intback_analog(void)
{
	uint8_t iregs[3] = { 0x01, 0x08, 0xF0 };
	trace_mark("intback_analog (3dpad analog, all buttons down)");
	setup_common(0x5, TB_CLOCK_NTSC_352, false);
	/* Bit 12 selects analog mode; bits 0..11 are the digital buttons, and
	 * set means pressed -- so 0x1FFF is analog mode with every button down,
	 * which is what this scenario is named after.  The thumbsticks and
	 * triggers go to their extremes so the axes are distinguishable from
	 * centre in the report. */
	tb_pad_set_buttons(&g_ports[0][TB_PAD_BTN], 0x1FFF);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_THUMBX], 0x0000);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_THUMBY], 0xFFFF);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_SHLDL], 0xFFFF);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_SHLDR], 0xFFFF);
	tb_backend->set_input(0, "3dpad", g_ports[0]);
	tb_backend->set_input(1, "none", g_ports[1]);
	tb_backend->reset(false);
	tb_backend->update_input(1000);
	intback_exchange(0x10, iregs, 3);
	tb_advance(200000);
	dump_regs();
}

/* ---- scenario 4: mouse ---- */
static void sc_intback_mouse(void)
{
	uint8_t iregs[3] = { 0x01, 0x08, 0xF0 };
	trace_mark("intback_mouse");
	setup_common(0x5, TB_CLOCK_NTSC_352, true);
	tb_backend->set_input(0, "mouse", g_ports[0]);
	tb_backend->set_input(1, "none", g_ports[1]);
	tb_backend->reset(false);
	tb_backend->update_input(1000);
	intback_exchange(0x10, iregs, 3);
	tb_advance(200000);
	dump_regs();
}

/* ---- scenario 5: multi-tap with three sub-devices ---- */
static void sc_intback_multitap(void)
{
	uint8_t iregs[3] = { 0x01, 0x08, 0xF0 };
	trace_mark("intback_multitap (3 subs)");
	setup_common(0x5, TB_CLOCK_NTSC_352, true);
	/* Three pads behind a multi-tap on port 0, each with a different
	 * button pattern so the reports can be told apart.  Bit 12 puts each
	 * one in analog mode, so each sub-slot carries the full 6-byte packet. */
	for (int i = 0; i < 3; i++)
		tb_pad_set_buttons(&g_ports[i][TB_PAD_BTN], (uint16_t)(0x1000 | (i + 1)));
	tb_backend->set_input(0, "3dpad", g_ports[0]);
	tb_backend->set_input(1, "3dpad", g_ports[1]);
	tb_backend->set_input(2, "3dpad", g_ports[2]);
	tb_backend->set_input(3, "none", g_ports[3]);
	tb_backend->set_multitap(0, true);
	tb_backend->reset(false);
	tb_backend->update_input(1000);
	intback_exchange(0x10, iregs, 3);
	tb_advance(200000);
	dump_regs();
}

/* ---- scenario 5b: digital mode with an asymmetric button pattern ---- */
static void sc_intback_digital_buttons(void)
{
	uint8_t iregs[3] = { 0x01, 0x08, 0xF0 };
	trace_mark("intback_digital_buttons (3dpad digital, buttons held)");
	setup_common(0x5, TB_CLOCK_NTSC_352, false);
	/* Bit 12 clear means digital mode, so the report is just the Saturn
	 * Control Pad's two button bytes and nothing else.  The pattern is
	 * deliberately asymmetric -- 0x0A35 puts a different nybble in every
	 * one of the four groups -- because the all-released and all-pressed
	 * patterns are symmetric and so cannot tell whether the report packs
	 * bits 0..3 into the high nybble or the low one.
	 *
	 * The thumbsticks and triggers sit at centre: a shoulder far enough
	 * from centre sets bit 11 or bit 15 through the hysteresis in
	 * iodev.c, which would move these bytes behind the scenario's back. */
	tb_pad_set_buttons(&g_ports[0][TB_PAD_BTN], 0x0A35);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_THUMBX], 0x8000);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_THUMBY], 0x8000);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_SHLDL], 0x8000);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_SHLDR], 0x8000);
	tb_backend->set_input(0, "3dpad", g_ports[0]);
	tb_backend->set_input(1, "none", g_ports[1]);
	tb_backend->reset(false);
	tb_backend->update_input(1000);
	intback_exchange(0x10, iregs, 3);
	tb_advance(200000);
	dump_regs();
}

/* ---- scenario 5c: first-generation digital gamepad ---- */
static void sc_intback_gamepad(void)
{
	uint8_t iregs[3] = { 0x01, 0x08, 0xF0 };
	trace_mark("intback_gamepad (first-generation pad)");
	setup_common(0x5, TB_CLOCK_NTSC_352, false);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_BTN], 0x050A);
	tb_backend->set_input(0, "gamepad", g_ports[0]);
	tb_backend->set_input(1, "none", g_ports[1]);
	tb_backend->reset(false);
	tb_backend->update_input(1000);
	intback_exchange(0x10, iregs, 3);
	tb_advance(200000);
	dump_regs();
}

/* ---- scenario 5d: multi-tap on port 1 ---- */
static void sc_intback_multitap_p1(void)
{
	uint8_t iregs[3] = { 0x01, 0x08, 0xF0 };
	trace_mark("intback_multitap_p1 (tap on port 1)");
	setup_common(0x5, TB_CLOCK_NTSC_352, false);
	/* A tap on port 1 rather than port 0.  Two things this pins down:
	 * port_device_index() has to leave port 0 on the first virtual slot
	 * instead of letting the tap swallow it, and the tap's own sub-slot
	 * cursor starts at slot 1 rather than slot 0 -- Beetle's MapPorts()
	 * walks one cursor across both ports, so a tap on port 1 begins
	 * exactly where port 0 left off.
	 *
	 * The sub-pads are in digital mode on purpose: the multi-tap's
	 * id1 == 0xB branch is a different packet from its analog one and no
	 * other scenario takes it. */
	tb_pad_set_buttons(&g_ports[0][TB_PAD_BTN], 0x0A05);
	for (int i = 1; i < 4; i++)
		tb_pad_set_buttons(&g_ports[i][TB_PAD_BTN],
		                   (uint16_t)(i * 0x0331));
	tb_backend->set_input(0, "3dpad", g_ports[0]);
	tb_backend->set_input(1, "3dpad", g_ports[1]);
	tb_backend->set_input(2, "3dpad", g_ports[2]);
	tb_backend->set_input(3, "3dpad", g_ports[3]);
	tb_backend->set_input(4, "none", g_ports[4]);
	tb_backend->set_input(5, "none", g_ports[5]);
	tb_backend->set_multitap(1, true);
	tb_backend->reset(false);
	tb_backend->update_input(1000);
	intback_exchange(0x10, iregs, 3);
	tb_advance(200000);
	dump_regs();
}

/* ---- scenario 6: SETTIME / SETSMEM round trip ---- */
static void sc_settime_smem(void)
{
	uint8_t iregs[7] = { 0x08, 0x03, 0x10, 0x11, 0x12, 0x13, 0x14 };
	uint8_t stat[3] = { 0x01, 0x00, 0xF0 };
	trace_mark("settime+setsmem then read back");
	setup_common(0xC /* EU PAL */, TB_CLOCK_PAL_352, true);
	issue(0x16, iregs, 7);
	wait_sf(TB_SF_DEADLINE);
	tb_advance(100000);
	issue(0x17, iregs, 7);
	wait_sf(TB_SF_DEADLINE);
	tb_advance(100000);
	intback_exchange(0x10, stat, 3);
	tb_advance(200000);
	dump_regs();
}

/* ---- scenario 7: the finicky-game command set ---- */
static void sc_command_matrix(void)
{
	static const uint8_t cmds[] = {
		0x00, 0x02, 0x03, 0x06, 0x07, 0x08, 0x09, 0x18, 0x19, 0x1A
	};
	char label[64];

	trace_mark("command_matrix (reset, slave, sound, CD, nmi, reset-enable)");
	setup_common(0x5, TB_CLOCK_NTSC_352, true);
	for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
		snprintf(label, sizeof label, "cmd %02X", cmds[i]);
		trace_mark(label);
		issue(cmds[i], NULL, 0);
		wait_sf(TB_SF_DEADLINE);
		tb_advance(100000);
		dump_regs();
	}
}

/* ---- scenario 8: SH-2 direct mode (PDR/DDR/IOSEL) ---- */
static void sc_direct_mode(void)
{
	uint8_t iregs[3] = { 0x00, 0x08, 0xF0 };
	trace_mark("direct_mode (SH2 pokes the pad port directly)");
	setup_common(0x5, TB_CLOCK_NTSC_352, true);
	tb_backend->set_input(0, "3dpad", g_ports[0]);
	tb_backend->set_input(1, "none", g_ports[1]);
	tb_backend->reset(false);
	tb_backend->update_input(1000);

	/* 3D pad in analog mode so the full packet is served. */
	tb_pad_set_buttons(&g_ports[0][TB_PAD_BTN], 0x1000);

	tb_write(g_ts, TB_IOSEL, 0x00);	/* SMPC-mediated mode */
	tb_write(g_ts, TB_DDR1, 0x40);	/* TH as output */
	tb_write(g_ts, TB_PDR1, 0x40);
	tb_advance(1000);
	trace_reg_byte("PDR1", tb_read(g_ts, TB_PDR1));

	tb_write(g_ts, TB_PDR1, 0x00);
	tb_advance(1000);
	trace_reg_byte("PDR1", tb_read(g_ts, TB_PDR1));

	tb_write(g_ts, TB_DDR1, 0x60);	/* TH + TR as outputs */
	for (int i = 0; i < 8; i++) {
		static const uint8_t thtr[] = { 0x60, 0x20, 0x40, 0x00,
		                                0x60, 0x20, 0x40, 0x00 };
		tb_write(g_ts, TB_PDR1, thtr[i]);
		tb_advance(1000);
		trace_reg_byte("PDR1", tb_read(g_ts, TB_PDR1));
	}

	tb_write(g_ts, TB_IOSEL, 0x01);	/* SH-2 direct mode */
	tb_write(g_ts, TB_PDR1, 0x7F);
	tb_advance(1000);
	trace_reg_byte("PDR1", tb_read(g_ts, TB_PDR1));

	/* And confirm INTBACK still works afterwards. */
	tb_write(g_ts, TB_IOSEL, 0x00);
	intback_exchange(0x10, iregs, 3);
	tb_advance(200000);
	dump_regs();
}

/* ---- scenario 8b: the RTC actually ticking ---- */
static void sc_intback_rtc_tick(void)
{
	uint8_t iregs[3] = { 0x01, 0x00, 0xF0 };

	/* The one behaviour of the RTC that nothing else here checks: that it
	 * rolls over on its own, in BCD, on a vblank boundary.  rtc_inc_second()
	 * runs on every scenario -- sysres_ckchg advances four seconds' worth --
	 * but until now no assertion could see the result, because the
	 * scenarios that report the status block advance under a second and the
	 * ones that advance enough never report.
	 *
	 * Frames have to be run one at a time.  do_vblank_housekeeping() is
	 * where the clock is drained, so a single large tb_advance() moves
	 * model time without ever producing a vblank edge and the clock stands
	 * still -- which is what made the first version of this scenario read
	 * the same second at 4 000 000 ticks as at 1 700 000.
	 *
	 * 330 NTSC frames.  Five seconds of the clock have passed, because 330
	 * frames is 5.5 s of video at 60 Hz and the RTC is whole seconds.  The
	 * margin is lopsided and worth stating: the model is in its power-on
	 * 26 MHz mode while the frames claim 28 MHz, so a frame is worth 1.0654
	 * model seconds and 330 of them is 5.858, not 5.5 -- 0.86 past the
	 * five-second mark, but only 0.14 short of six.
	 *
	 * Measured rather than assumed, and the two sides are not alike: 0x10
	 * holds from 285 frames to 335, so this sits 47 frames clear of the
	 * lower boundary and only 8 from the upper.  Those 8 frames are 0.14 s.
	 * The count is a literal and the run is deterministic, so a thin margin
	 * is not fragile here -- it only means a future change to the harness's
	 * clock calibration would have to re-derive this expectation rather than
	 * leave it alone.  The same double check is what pins the calibration
	 * that used to be 61x out: before the fix the scenario read 61.2x fast.
	 */
	trace_mark("intback_rtc_tick (the RTC rolls over, in BCD, on its own)");
	setup_common(0x5, TB_CLOCK_NTSC_352, true);
	tb_backend->set_input(0, "none", g_ports[0]);
	tb_backend->set_input(1, "none", g_ports[1]);
	tb_backend->reset(false);

	for (int i = 0; i < 330; i++) {
		tb_backend->start_frame();
		run_frame();
	}

	/* Status report only -- IREG1 = 0x00 is what asks for it.  The
	 * peripheral half would overwrite the head of the block, and the RTC is
	 * at OREG[1..7], which the status half leaves alone. */
	intback_exchange(0x10, iregs, 3);
	tb_advance(1000);
	dump_regs();
}

/* ---- scenario 8c: the watch crystal's tolerance ---- */
static void sc_intback_rtc_oscillator(void)
{
	uint8_t iregs[3] = { 0x01, 0x00, 0xF0 };

	/* The core oscillator and the 32.768 kHz watch crystal are separate
	 * parts, and the RTC's second is the watch crystal's 32 768 cycles, not
	 * 4 000 000 of core clock.  On a console the two drift apart at their
	 * own rates; the model has one timebase, so smpc_set_rtc_oscillator()
	 * is what lets it say which one it is imitating.
	 *
	 * 50 000 ppm is 5%, which is far worse than a real tuning-fork crystal
	 * (~20 ppm).  It is exaggerated on purpose: at 5% fifty seconds of
	 * running time already separates the two readings by a couple of whole
	 * seconds, so the effect is unmistakable in a test that runs in a
	 * second.  At a realistic 20 ppm the same gap needs four minutes of
	 * model time, and the harness cannot reach that -- see the note on frame
	 * count below.
	 *
	 * 4 200 NTSC frames, and the arithmetic in full because the obvious
	 * version of it is wrong here.  A frame is 477 082 master ticks, so
	 * 4 200 frames is 70 s of *video*.  But the model is in its power-on
	 * 26 MHz mode -- nothing has issued CKCHG352 -- while the frames claim
	 * 28 MHz, so host ticks convert to core clocks at 4e6 * 65 / (28 636 364
	 * * 61) = 0.148845 rather than 0.139670, a frame is worth 1.0654 model
	 * seconds, and 4 200 frames is 74.55 model seconds, not 70.  At
	 * +50 000 ppm the watch crystal runs 5% fast, so its second is
	 * 3 800 000 core clocks and the same interval is 78.485.
	 *
	 * Expect 15:05:23, not the 15:05:19 a nominal crystal reports.  The
	 * margin is asymmetric and small: 78.485 is 0.485 past the 78-second
	 * mark and only 0.515 short of 79, which at 0.0187 model seconds per
	 * frame is a window 26 frames wide either side.  Measured: 4 180 and
	 * 4 220 frames both report 15:05:23; 4 170 reports 15:05:22 and 4 230
	 * already 15:05:24.  The first choice of 3 000 frames was not usable --
	 * it landed on 56.06, a hundredth of a second past a boundary.
	 *
	 * The frame count is bounded by the harness's int32 timestamp: 4 500
	 * frames is the ceiling before g_ts overflows and tb_advance_to()
	 * reports that time went backwards.  That is why the ppm is exaggerated
	 * rather than the run lengthened.
	 */
	trace_mark("intback_rtc_oscillator (+50000 ppm on the watch crystal)");
	setup_common(0x5, TB_CLOCK_NTSC_352, true);
	tb_backend->set_input(0, "none", g_ports[0]);
	tb_backend->set_input(1, "none", g_ports[1]);
	tb_backend->set_rtc_oscillator(50000);
	tb_backend->reset(false);

	for (int i = 0; i < 4200; i++) {
		tb_backend->start_frame();
		run_frame();
	}


	intback_exchange(0x10, iregs, 3);
	tb_advance(1000);
	dump_regs();
}

/* ---- scenario 9: SYSRES and CKCHG352, which span many frames ---- */
static void sc_sysres_ckchg(void)
{
	trace_mark("sysres + ckchg352 (multi-vblank stalls)");
	setup_common(0x5, TB_CLOCK_NTSC_352, true);
	issue(0x0D, NULL, 0);			/* SYSRES */
	wait_sf(TB_SF_DEADLINE);
	tb_advance(500000);
	trace_mark("after sysres");

	issue(0x0E, NULL, 0);			/* CKCHG352 */
	tb_backend->start_frame();
	tb_advance(2000000);
	/* A clock change stalls the console for several frames, so give it
	 * real frames to complete in. */
	for (int i = 0; i < 10; i++) {
		tb_backend->start_frame();
		run_frame();
	}
	wait_sf(TB_SF_DEADLINE);
	tb_advance(500000);
	dump_regs();
}

const TbScenario tb_scenarios[] = {
	{ "status_only",        sc_status_only },
	{ "intback_one_pad",    sc_intback_one_pad },
	{ "intback_analog",     sc_intback_analog },
	{ "intback_mouse",      sc_intback_mouse },
	{ "intback_multitap",   sc_intback_multitap },
	{ "intback_digital_buttons", sc_intback_digital_buttons },
	{ "intback_gamepad",    sc_intback_gamepad },
	{ "intback_multitap_p1", sc_intback_multitap_p1 },
	{ "intback_rtc_tick",   sc_intback_rtc_tick },
	{ "intback_rtc_oscillator", sc_intback_rtc_oscillator },
	{ "settime_smem",       sc_settime_smem },
	{ "command_matrix",     sc_command_matrix },
	{ "direct_mode",        sc_direct_mode },
	{ "sysres_ckchg",       sc_sysres_ckchg },
};

const int tb_scenario_count = (int)(sizeof tb_scenarios / sizeof tb_scenarios[0]);
