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
static uint32_t g_last_irq_count;
static uint32_t g_irq_seen;

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

	/* Digital-mode neutral: 12 digital bits set (released), analog bit
	 * clear.  The 3D pad maps a 16-bit axis of 0x8000 to 0x80. */
	for (int i = 0; i < 6; i++) {
		tb_pad_set_buttons(&g_ports[i][TB_PAD_3DPAD + 0], 0x0FFF);
		tb_pad_set_buttons(&g_ports[i][TB_PAD_3DPAD + 2], 0x8000);
		tb_pad_set_buttons(&g_ports[i][TB_PAD_3DPAD + 4], 0x8000);
	}
	(void)digital_pad;

	tb_backend->init(area, g_master_clock, false);
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
	/* dbuttons bit12 set = analog mode; all 12 digital bits clear = pressed */
	tb_pad_set_buttons(&g_ports[0][TB_PAD_3DPAD + 0], 0x1000);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_3DPAD + 2], 0x0000);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_3DPAD + 4], 0xFFFF);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_3DPAD + 6], 0xFFFF);
	tb_pad_set_buttons(&g_ports[0][TB_PAD_3DPAD + 8], 0xFFFF);
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
	/* Three pads behind a multi-tap on port 1, each with a different
	 * button pattern so the reports can be told apart. */
	for (int i = 0; i < 3; i++)
		tb_pad_set_buttons(&g_ports[i][TB_PAD_3DPAD], (uint16_t)(0x1000 | (i + 1)));
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
	tb_pad_set_buttons(&g_ports[0][TB_PAD_3DPAD + 0], 0x1000);

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
	{ "settime_smem",       sc_settime_smem },
	{ "command_matrix",     sc_command_matrix },
	{ "direct_mode",        sc_direct_mode },
	{ "sysres_ckchg",       sc_sysres_ckchg },
};

const int tb_scenario_count = (int)(sizeof tb_scenarios / sizeof tb_scenarios[0]);
