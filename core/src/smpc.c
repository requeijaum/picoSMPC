/*
 * smpc.c -- SMPC core: registers, command engine, INTBACK report engine.
 *
 * The command engine is a resumable state machine rather than a straight-line
 * function, because a single INTBACK spans a whole vblank: the SMPC writes
 * OREG one nibble at a time, waits for the host to acknowledge each block,
 * and aborts the whole exchange if vblank ends first.  Expressing that with
 * a coroutine is not a stylistic choice -- an instantaneous implementation
 * is what makes games hang (Ymir's dev-notes/finicky-games/smpc-timings.txt
 * documents four titles that break without the real delays, and one that
 * breaks without the real *timeouts*).
 *
 * The state machine is a direct transliteration of Mednafen's, because that
 * is the only reference in the tree that models this at cycle level, and
 * because sim/ can diff the two byte for byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "smpc/smpc.h"

#define SMPC_CLOCK_HZ 4000000

/* SMPC clocks, from Mednafen's mednafen/ss/smpc.c. */
#define EAT_COMMAND_LATENCY 92		/* before a command takes effect */
#define EAT_STATUS_REPORT 952
#define EAT_SETTIME 380
#define EAT_SETSMEM 234
#define EAT_VBLANK_HOUSEKEEPING 234
#define EAT_ABORT 87
#define EAT_REPORT_PRELUDE 120
#define EAT_PORT_PREAMBLE 380
#define EAT_NYBBLE 21		/* per OREG nybble */
#define EAT_NYBBLE_SETTLE 50	/* per pad-port half-nybble */
#define EAT_DIGITAL_GAP 30
#define EAT_PORT_TAIL 26
/* How many consecutive polls to wait for the peripheral to toggle TL before
 * giving up on a nybble.  A real pad answers in a few microseconds; this is
 * roughly 2 ms of master clock, i.e. two orders of magnitude of headroom
 * before a stuck peripheral is declared dead. */
#define TL_POLL_LIMIT 48

#define POLL_SHORT 8		/* master clocks */
#define POLL_COND 1000

/* ---- state machine phases ---- */
enum
{
	PH_WAIT_PENDING = 0,
	PH_VBLANK_HOUSEKEEPING,
	PH_TAKE_COMMAND,
	PH_COMMAND_BODY,
	/* INTBACK, status report */
	PH_STATUS_WAIT,
	/* INTBACK, peripheral report */
	PH_PENDING_CONT_A,
	PH_PENDING_CONT_B,
	PH_WAIT_NOT_VB,
	PH_OPT_WAIT,
	PH_REPORT_PRELUDE,
	PH_PORT_PRELUDE,
	PH_PORT_SKIP,
	PH_ID1_HI, PH_ID1_HI_SETTLE,
	PH_ID1_LO, PH_ID1_LO_SETTLE,
	PH_DIG_HI, PH_DIG_HI_SETTLE,
	PH_DIG_LO, PH_DIG_LO_SETTLE,
	PH_DIG_GAP,
	PH_DIG_NYB_0, PH_DIG_NYB_0_SETTLE,
	PH_DIG_NYB_1, PH_DIG_NYB_1_SETTLE,
	PH_DIG_NYB_2, PH_DIG_NYB_2_SETTLE,
	PH_DIG_NYB_3, PH_DIG_NYB_3_SETTLE,
	PH_DIG_NYB_4, PH_DIG_NYB_4_SETTLE,
	PH_DIG_NYB_5, PH_DIG_NYB_5_SETTLE,
	PH_DIG_NYB_6, PH_DIG_NYB_6_SETTLE,
	PH_DIG_NYB_7, PH_DIG_NYB_7_SETTLE,
	PH_ANALOG_ID_HI, PH_ANALOG_ID_HI_SETTLE,
	PH_ANALOG_ID_LO, PH_ANALOG_ID_LO_SETTLE,
	PH_TAP_ID_HI, PH_TAP_ID_HI_SETTLE,
	PH_TAP_ID_LO, PH_TAP_ID_LO_SETTLE,
	PH_TAP_SLOT_HEAD, PH_TAP_SLOT_HEAD_SETTLE,
	PH_TAP_SLOT_TAIL, PH_TAP_SLOT_TAIL_SETTLE,
	PH_TAP_READ_HI, PH_TAP_READ_HI_SETTLE,
	PH_TAP_READ_LO, PH_TAP_READ_LO_SETTLE,
	PH_TAP_WRITE_HEAD, PH_TAP_WRITE_HEAD_SETTLE,
	PH_TAP_WRITE_0, PH_TAP_WRITE_0_SETTLE,
	PH_TAP_WRITE_1, PH_TAP_WRITE_1_SETTLE,
	PH_TAP_WRITE_2, PH_TAP_WRITE_2_SETTLE,
	PH_TAP_WRITE_3, PH_TAP_WRITE_3_SETTLE,
	PH_UNKNOWN_ID_0, PH_UNKNOWN_ID_1,
	PH_PORT_TAIL2,
	PH_REPORT_DONE,
	PH_ABORT,
	PH_CMD_DONE
};

/* ------------------------------------------------------------------ */
/* Report sequencer                                                    */
/* ------------------------------------------------------------------ */

/*
 * The peripheral half of INTBACK is not straight-line code: it contains
 * loops whose trip count is only discovered over the wire (how many pads a
 * multi-tap has, how many data bytes each one reports), and it suspends
 * constantly -- once per OREG nybble, once per pad-port half-nybble, and
 * once per 32-nybble block while it waits for the host to say "continue".
 *
 * Writing that as a switch with one case per suspension point is possible
 * (Mednafen does it, by flattening a computed-goto coroutine with the
 * preprocessor) but it is unreadable and easy to get subtly wrong, because
 * every suspension point has to remember where to resume.  Instead the body
 * is emitted as a tiny instruction stream and interpreted, so suspending is
 * literally "return, with the program counter left where it is".
 *
 * The stream is built incrementally: the interpreter stops when it runs off
 * the end, and a multi-tap's data-byte count -- which is not known until the
 * tap has been interrogated -- simply appends more instructions when it is
 * learned.  `smpc_report_step` therefore takes no explicit trip counts.
 */
enum
{
	OP_END = 0,
	OP_EAT,		/* arg = SMPC clocks                              */
	OP_SETTHTR,	/* arg = TH level | TR level << 1                 */
	OP_SAMPLE,	/* arg = index into jr.work                       */
	OP_WAIT_TL,	/* arg = wanted TL level                          */
	OP_NYB,		/* arg = source descriptor (see SRC_*)            */
	OP_REPEAT,	/* arg = unused                                    */
	OP_ENDREPEAT,	/* arg = counter                                   */
	OP_GOTO,	/* arg = target pc                                 */
	OP_SET_CTRMAX,	/* arg = counter | source << 2 | clamp6 << 4       */
	OP_IF_PORT_SKIP,/* arg = counter(port) | target << 2              */
	OP_IF_ID1,	/* arg = expected | target << 4                    */
	OP_IF_TAP,	/* arg = target_if_tap | target_if_not_tap << 8    */
	OP_IF_CTR_NONZERO,	/* arg = counter | target << 8                  */
	OP_SET_ID1,	/* id1 = de-scrambled pair of work[0], work[1]      */
	OP_SET_ID2,	/* id2 = nybble(work[0]) << 4 | nybble(work[1])    */
	OP_SET_ID2_TAP,	/* id2 from a sub-slot; leaves is_tap alone    */
	OP_SET_IDTAP,	/* id_tap = (id2 & 0xF) << 4 | nybble(work[2])     */
	OP_SET_IDTAP_DIRECT,	/* id_tap = 0xF1                           */
	OP_FORCE_MOUSE_ID,	/* arg = the id1 that implies a mouse        */
	OP_STORE_DATABYTE,	/* read_buffer[ctr] = work[0..1], ctr++      */
	OP_LOAD_DATABYTE,	/* work[0] = read_buffer[ctr], ctr++          */
	OP_NEXT_PORT,	/* advance to the next front-panel port          */
	OP_IF_MULTI,	/* arg = target when a multi-tap IS present      */
	OP_IF_NOT_MULTI,	/* arg = target when there is NO multi-tap      */
	OP_IF_CTRMAX_ZERO,	/* arg = counter | target << 8; taken when the  */
				/* loop bound is 0 (a do-while would still run)  */
	OP_IF_TAP_LE1		/* arg = target when a tap has <= 1 sub-device  */
};

/* Counter registers for OP_ENDREPEAT / OP_SET_CTRMAX. */
enum
{
	CTR_PORT = 0,	/* the two front-panel ports         */
	CTR_TAP,		/* sub-devices behind a multi-tap   */
	CTR_DATA		/* data bytes of one sub-device     */
};

/* Sources for OP_SET_CTRMAX. */
enum
{
	CTRMAX_ID2 = 0,		/* id2 & 0xF  -- data bytes in one sub-device */
	CTRMAX_IDTAP,		/* id_tap & 0xF -- sub-devices behind a tap    */
	CTRMAX_PORTS		/* a literal: the two front-panel ports       */
};

/*
 * Sources for OP_NYB.  A nybble is one of: a constant, one half of id1 /
 * id2 / id_tap, or one nybble of a byte in jr.work[], optionally OR'd with 7.
 */

enum
{
	SRC_CONST = 0,
	SRC_REG_HI,	/* arg = register index (0 = id1, 1 = id2) */
	SRC_REG_LO,
	SRC_WORK,	/* arg = jr.work index                        */
	SRC_WORK_OR7	/* arg = jr.work index, nybble | 0x7            */
};

#define OPBUF 512
#define SMPC_LOOP_FRAMES 4

/* Front-panel controller ports. */
#define SMPC_PORT_COUNT 2

typedef struct
{
	uint8_t  op;
	uint16_t arg;
} SmpcOp;


struct Smpc
{
	SmpcEnv env;

	uint8_t  ireg[7];
	uint8_t  oreg[0x20];
	uint8_t  sr;
	bool     sf;
	uint8_t  bus_buffer;

	/* Persisted settings (NVRAM on hardware). */
	uint8_t  smem[4];
	uint8_t  area_code;

	/* Real-time clock: BCD, packed exactly as it appears in the status
	 * report, because the SMPC copies these bytes out wholesale. */
	uint8_t  rtc_valid;
	uint8_t  rtc_raw[7];

	bool     sound_cpu_on;
	bool     slave_sh2_on;
	int8_t   slave_sh2_pending;	/* -1 off, 0 none, 1 on */
	bool     cd_on;
	bool     reset_nmi_enabled;
	bool     reset_button;
	int32_t  reset_button_count;
	bool     reset_pending;
	int32_t  pending_clock_divisor;
	int32_t  current_clock_divisor;
	int32_t  master_clock;
	bool     block_sound_cpu_control;

	bool     vb;
	bool     vsync;
	bool     pending_vb;

	int32_t  last_ts;
	int64_t  clock_ratio;		/* 32.32: master clocks -> SMPC clocks */
	int64_t  clock_counter;
	uint64_t rtc_clock_accum;

	int32_t  pending_command;	/* command byte, -1 when idle */
	int32_t  executing_command;
	int32_t  phase;

	/* Watchdog for the IREG0 write that starts/continues/breaks INTBACK.
	 * Only a change in the two handshake bits should re-arm the exchange. */
	uint8_t  ir0wx, ir0wa;

	/* ---- report engine ---- */
	struct
	{
		int64_t  time_counter;
		int32_t  start_time;
		int32_t  opt_wait_until_time;
		int32_t  opt_eat_time;
		int32_t  opt_read_time;

		uint8_t  mode[2];
		bool     time_opt_en;
		bool     next_cont_bit;

		uint8_t  cur_port;
		uint8_t  id1, id2, id_tap;
		uint8_t  owp;		/* OREG write pointer, 6 bits of nybbles */

		uint8_t  work[8];
		uint8_t  tap_counter, tap_count;
		uint8_t  read_counter, read_count;
		uint8_t  read_buffer[256];
		uint8_t  write_counter;
		int32_t  pd_counter;

		/* Report sequencer state. */
		/* loop_start is indexed by nesting depth, and the report nests
		 * three deep: front-panel port, sub-device behind a multi-tap,
		 * and data bytes within a sub-device. */
		SmpcOp    prog[OPBUF];
		uint8_t   tl_timeout;	/* bounded wait on the peripheral's TL */
		int32_t   ckchg_vb;
		uint16_t  prog_len;
		uint16_t  pc;
		/* One frame per nesting level: port -> sub-device -> data. */
		uint16_t  loop_start[4];
		int       loop_depth;
		uint16_t  ctr[4];
		uint16_t  ctr_max[4];
		bool      host_pending;
		bool      is_tap;
		int32_t   wake;
	} jr;

	/* ---- controller ports ---- */
	struct SmpcPort
	{
		/* No cached device pointer: a port without a multi-tap always
		 * uses s->devices[port].  Caching it meant that replacing a
		 * device left the port pointing at freed memory. */
		SmpcMultiTap  *tap;
		bool           tap_enabled;
		bool           owns_dev;
		/*
		 * Two register sets per port, selected by IOSEL.
		 *
		 * This is not two registers -- there is one PDR and one DDR.  It
		 * is the pin mux: index 0 holds the values the SMPC's own
		 * peripheral driver is putting on TH and TR, and index 1 holds
		 * whatever the SH-2 last wrote.  Which one reaches the pin is
		 * IOSEL's job.  So a host write to PDR is *inert* while the SMPC
		 * is driving the port, and the SMPC's own TH/TR are inert while
		 * the host is.  Titles that poke the port directly rely on that,
		 * and a single shared register cannot express it.
		 */
		uint8_t        data_out[2];
		uint8_t        data_dir[2];
		bool           direct_mode_en;
		bool           ex_latch_en;
		uint8_t        io_state;
	} port[2];

	SmpcIoDev *devices[6];
	bool       owns_device[6];
};
/* ------------------------------------------------------------------ */
/* Environment helpers                                                 */
/* ------------------------------------------------------------------ */

/*
 * The SCU interrupt lines are driven as levels and latched into a status
 * register on the far side, so the host sees edges.  The core does not
 * filter: it reports every change of level, and the testbench's trace does
 * the transition filtering, so the same rule is applied identically to the
 * golden model and to this one.
 */
static void env_scu_int(Smpc *s, unsigned which, bool active)
{
	if (s->env.scu_int)
		s->env.scu_int(s->env.ctx, which, active);
}

/* The SMPC's "System Manager" interrupt is a pulse, not a level: it goes
 * high and immediately low, and the SH-2 latches it in the SCU's ISTAT. */
static void pulse_system_manager_irq(Smpc *s)
{
	env_scu_int(s, SMPC_SCU_INT_SMPC, true);
	env_scu_int(s, SMPC_SCU_INT_SMPC, false);
}

/* NMIREQ, CKCHG and the reset button all generate a pulse: low then high.
 * The SH-2 samples the edge, so both halves matter. */
static void raise_master_nmi(Smpc *s)
{
	if (s->env.sh2_nmi) {
		s->env.sh2_nmi(s->env.ctx, 0, false);
		s->env.sh2_nmi(s->env.ctx, 0, true);
	}
}

/*
 * Power-on and reset assert NMI as a *level* and leave it there; the SH-2's
 * own reset is what clears it.  Modelling that as a pulse would look almost
 * identical at the pin but differs in one observable way -- whether the line
 * is still asserted afterwards -- so it is kept separate.
 */
static void assert_master_nmi(Smpc *s)
{
	if (s->env.sh2_nmi)
		s->env.sh2_nmi(s->env.ctx, 0, true);
}

void smpc_set_reset_button(Smpc *s, bool pressed)
{
	s->reset_button = pressed;
}

/* ------------------------------------------------------------------ */
/* RTC                                                                 */
/* ------------------------------------------------------------------ */

/*
 * The seven bytes of the status report are the RTC's own byte layout, so
 * they are modelled as a union of that layout and named fields.  Order and
 * packing must not change: the SMPC copies these seven bytes straight into
 * OREG1..OREG7.
 *
 *   [0] year century+decades (BCD)  [1] year tens+units (BCD)
 *   [2] weekday (high nybble) | month (low nybble, hex)
 *   [3] day of month (BCD)          [4] hour  [5] minute  [6] second
 */
typedef union
{
	uint8_t raw[7];
	struct
	{
		uint8_t year[2];
		uint8_t wday_mon;
		uint8_t mday;
		uint8_t hour;
		uint8_t minute;
		uint8_t second;
	};
} SmpcRtc;

static uint8_t bcd_inc(uint8_t v)
{
	unsigned tmp = (unsigned)(v & 0xF) + 1;

	if (tmp >= 0xA)
		tmp += 0x6;
	tmp += v & 0xF0;
	if (tmp >= 0xA0)
		tmp += 0x60;
	return (uint8_t)tmp;
}

/* Days in each month (BCD). */
static const uint8_t k_month_days[0x10] = {
	0x10, 0x31, 0x28, 0x31, 0x30, 0x31, 0x30, 0x31,
	0x31, 0x30, 0x31, 0x30, 0x31, 0xC1, 0xF5, 0xFF
};

static void rtc_inc_second(SmpcRtc *t)
{
	if (t->second == 0x59) {
		t->second = 0x00;
		if (t->minute == 0x59) {
			t->minute = 0x00;
			if (t->hour == 0x23) {
				t->hour = 0x00;

				/* Weekday is the high nybble, 0 = Sunday. */
				if (t->wday_mon >= 0x60)
					t->wday_mon &= 0x0F;
				else
					t->wday_mon += 0x10;

				{
					const unsigned mon = t->wday_mon & 0x0F;
					uint8_t limit = k_month_days[mon];

					/* February gains a day on a leap year; the test
					 * is "year mod 4, ignoring centuries". */
					if (mon == 0x02 &&
					    (t->year[1] & 0x1F) < 0x1A &&
					    !((t->year[1] + ((t->year[1] & 0x10) >> 3)) & 0x3))
						limit = (uint8_t)(limit + 1);

					if (t->mday >= limit) {
						t->mday = 0x01;
						if (mon == 0x0C) {
							t->wday_mon &= 0xF0;
							t->wday_mon |= 0x01;
							{
								const uint8_t y = bcd_inc(t->year[1]);
								t->year[1] = y;
								if (y == 0x00)
									t->year[0] = bcd_inc(t->year[0]);
							}
						} else {
							t->wday_mon++;
						}
					} else {
						t->mday = bcd_inc(t->mday);
					}
				}
			} else {
				t->hour = bcd_inc(t->hour);
			}
		} else {
			t->minute = bcd_inc(t->minute);
		}
	} else {
		t->second = bcd_inc(t->second);
	}
}

static uint8_t to_bcd(unsigned v)
{
	return (uint8_t)(((v / 10) << 4) | (v % 10));
}

void smpc_set_rtc(Smpc *s, const struct tm *ht, uint8_t lang)
{
	SmpcRtc *t = (SmpcRtc *)&s->rtc_raw[0];
	const unsigned year = (unsigned)(ht->tm_year + 1900);

	t->year[0] = to_bcd(year / 100);
	t->year[1] = to_bcd(year % 100);
	t->wday_mon = (uint8_t)(((ht->tm_wday & 0x7) << 4) | ((ht->tm_mon + 1) & 0xF));
	t->mday = to_bcd((unsigned)ht->tm_mday);
	t->hour = to_bcd((unsigned)ht->tm_hour);
	t->minute = to_bcd((unsigned)ht->tm_min);
	t->second = to_bcd((unsigned)ht->tm_sec);
	s->rtc_valid = true;
	s->smem[3] = lang;
}

void smpc_get_rtc(Smpc *s, uint8_t out[7])
{
	memcpy(out, s->rtc_raw, 7);
}

void smpc_set_smem(Smpc *s, const uint8_t smem[4])
{
	memcpy(s->smem, smem, 4);
}

void smpc_get_smem(Smpc *s, uint8_t out[4])
{
	memcpy(out, s->smem, 4);
}

void smpc_set_area_code(Smpc *s, uint8_t code)
{
	s->area_code = (uint8_t)(code & 0xF);
}

/* ------------------------------------------------------------------ */
/* Controller port bus                                                 */
/* ------------------------------------------------------------------ */

/*
 * Present the port's current state to the attached peripheral and latch what
 * comes back.
 *
 * The DDR convention is the opposite of what the register name suggests: a
 * 1 bit means the SMPC *drives* that line as an output, and a 0 means it
 * releases the line to float high.  So the value the peripheral sees is
 * (data_out | ~data_dir) & 0x7F, and the mask of lines currently driven is
 * data_dir itself.  Getting this backwards is not subtle -- a port
 * configured for output would float and a port configured for input would
 * fight the pad.
 *
 * The peripheral returns TL plus a 4-bit data nybble; TH, TR and the select
 * lines pass through whatever the SMPC asserted.  Bit 7 is never driven by
 * the peripheral.  A TH reading low while external latch is enabled is what
 * latches the light-gun crosshair and raises the PAD interrupt.
 */
/*
 * Which configured device answers on a physical port.
 *
 * Beetle walks one cursor over VirtualPorts: a tap on port 0 swallows six
 * entries, so port 1 lands on entry 6 and finds nothing there.  Returning
 * ~0u for "the port answers as the tap itself" and any value >= 6 for
 * "nothing plugged in" reproduces that without widening devices[].
 */
static unsigned port_device_index(const Smpc *s, unsigned port)
{
	unsigned vp = 0;

	for (unsigned sp = 0; sp < SMPC_PORT_COUNT; sp++) {
		const bool tap = s->port[sp].tap_enabled && s->port[sp].tap;

		if (sp == port)
			return tap ? ~(unsigned)0 : vp;
		vp += tap ? 6u : 1u;
	}
	return ~(unsigned)0;
}

static void update_io_bus(Smpc *s, unsigned port)
{
	struct SmpcPort *p;

	/* cur_port is driven by the sequencer, so it is bounds-checked here
	 * rather than trusted: an out-of-range value would index off the end
	 * of s->port[] and take the whole core with it. */
	if (port >= SMPC_PORT_COUNT)
		port = 0;
	p = &s->port[port];
	const unsigned sel = p->direct_mode_en ? 1u : 0u;
	const uint8_t data_dir = p->data_dir[sel];
	const uint8_t driven = (uint8_t)(data_dir & 0x7F);
	const uint8_t seen = (uint8_t)((p->data_out[sel] | (uint8_t)~data_dir) & 0x7F);
	uint8_t state;

	if (p->tap_enabled && p->tap)
		state = smpc_multitap_update_bus(p->tap, seen, driven);
	else {
		const unsigned di = port_device_index(s, port);

		if (di < 6 && s->devices[di])
			state = smpc_iodev_update_bus(s->devices[di], seen, driven);
		else
			state = (uint8_t)((seen & (driven | 0xE0)) | 0x7F);
	}

	p->io_state = state;

	if (p->ex_latch_en && !(state & SMPC_IO_TH)) {
		env_scu_int(s, SMPC_SCU_INT_PAD, true);
		if (s->env.ext_latch)
			s->env.ext_latch(s->env.ctx, true);
	}
}

/*
 * The SMPC's own pad-port driver.  `th` and `tr` are tri-state: -1 releases
 * the line to an input, where the weak pull-up on the Saturn side takes it
 * high.  Only the selected register set is touched, so a title that pokes the
 * port directly cannot corrupt the SMPC's own TH/TR drive.
 */
static void set_th_tr(Smpc *s, unsigned port, int th, int tr)
{
	struct SmpcPort *p = &s->port[port];
	const unsigned sel = p->direct_mode_en ? 1u : 0u;

	p->data_dir[sel] = (uint8_t)((((th >= 0) ? 1u : 0u) << 6) |
	                             (((tr >= 0) ? 1u : 0u) << 5));
	p->data_out[sel] = (uint8_t)((p->data_out[sel] & 0x1F) |
	                             (((th > 0) ? 1u : 0u) << 6) |
	                             (((tr > 0) ? 1u : 0u) << 5));
	update_io_bus(s, port);
}

#define CUR_PORT (s->port[s->jr.cur_port])
#define BS       (CUR_PORT.io_state)

/* Emitter.  Bounds are generous: the longest report is a 6-player multi-tap
 * with 15-byte packets, which is well under 500 instructions. */
static void emit(Smpc *s, uint8_t op, uint16_t arg)
{
	if (s->jr.prog_len >= OPBUF)
		return;
	s->jr.prog[s->jr.prog_len].op = op;
	s->jr.prog[s->jr.prog_len].arg = arg;
	s->jr.prog_len++;
}

/* The delay is stored in full: the port preamble alone is 380 SMPC clocks,
 * and clamping it to a byte would silently make the whole exchange 125
 * clocks short per port. */
static void emit_eat(Smpc *s, int n) { emit(s, OP_EAT, (uint16_t)n); }
/*
 * Each pad-port line is tri-state and all three states are distinct:
 * release to input (the weak pull-up takes it high), drive low, drive high.
 * A two-bit encoding is needed; collapsing release and low would make every
 * "drive TH low" read of the bus see an idle line instead.
 */
#define LINE_ENC(v) ((v) < 0 ? 0 : ((v) > 0 ? 2 : 1))

static void emit_set(Smpc *s, int th, int tr)
{
	emit(s, OP_SETTHTR, (uint16_t)((LINE_ENC(th) << 2) | LINE_ENC(tr)));
}

static int line_dec(int v)
{
	return (v & 3) == 0 ? -1 : ((v & 3) == 1 ? 0 : 1);
}
static void emit_sample(Smpc *s, int idx) { emit(s, OP_SAMPLE, (uint16_t)idx); }
/*
 * Sources are packed as kind | (index << 3), not kind | (index << 1).
 * Three bits of index and three of kind fits a nybble descriptor in a byte,
 * whereas one bit of kind does not: SRC_REG_LO is 2, so `desc & 1` read it
 * as SRC_CONST and every low nybble of a register decode came out as a
 * constant taken from the index field instead.
 */
static uint8_t pack_nyb(int kind, int index) { return (uint8_t)(kind | (index << 3)); }
static void emit_nyb_const(Smpc *s, int v) { emit(s, OP_NYB, pack_nyb(SRC_CONST, v)); }
static void emit_nyb_reg(Smpc *s, int reg, int high)
{
	emit(s, OP_NYB, pack_nyb(high ? SRC_REG_HI : SRC_REG_LO, reg));
}
static void emit_nyb_work(Smpc *s, int idx) { emit(s, OP_NYB, pack_nyb(SRC_WORK, idx)); }
static void emit_nyb_work_or7(Smpc *s, int idx) { emit(s, OP_NYB, pack_nyb(SRC_WORK_OR7, idx)); }

/* Register index for OP_NYB, packed as kind | (index << 3) where kind picks
 * the high or low nybble: 0 = id1, 1 = id2, 2 = id_tap. */
static uint8_t reg_byte(Smpc *s, int reg)
{
	switch (reg & 3) {
	case 0:  return s->jr.id1;
	case 1:  return s->jr.id2;
	default: return s->jr.id_tap;
	}
}

static uint8_t nyb_source(Smpc *s, uint8_t desc)
{
	const int kind = desc & 0x07;
	/* Four bits, not three: a constant nybble is 0..15 and the `F` of the
	 * `F1 02` report header is 0xF, so a 3-bit mask turned it into 0x7 and
	 * the id1 == 0xB branch -- the one generation that does not negotiate,
	 * reachable only from a plain gamepad -- reported 0x71 as its header.
	 * The register and work sources mask their own index to what they can
	 * address, so nothing else is affected. */
	const int arg = (desc >> 3) & 0x0F;

	switch (kind) {
	case SRC_CONST:    return (uint8_t)(arg & 0x0F);
	/* Mask with 3, not 1: index 2 is id_tap, and the multi-tap header
	 * emits both of its nybbles through these two cases.  Masking it to
	 * one bit silently replaced id_tap with id1, so the adapter header
	 * came out as the id nybble of whatever was plugged into the port. */
	case SRC_REG_HI:   return (uint8_t)(reg_byte(s, arg & 3) >> 4);
	case SRC_REG_LO:   return (uint8_t)(reg_byte(s, arg & 3) & 0x0F);
	case SRC_WORK:     return (uint8_t)(s->jr.work[arg & 7] & 0x0F);
	case SRC_WORK_OR7: return (uint8_t)((s->jr.work[arg & 7] & 0x0F) | 0x7);
	default:           return 0;
	}
}

/* Complete a block-boundary host handshake that is already in progress, or
 * report that we are still waiting.  Split out so OP_NYB can suspend and
 * resume without re-pulsing the interrupt. */
static int host_continue(Smpc *s)
{
	if (((bool)(s->ireg[0] & 0x80)) != s->jr.next_cont_bit ||
	    (s->ireg[0] & 0x40)) {
		s->jr.wake = POLL_COND;
		return 1;			/* still waiting */
	}
	if (s->ireg[0] & 0x40)
		return 2;				/* host broke out */
	s->ir0wa = 0;
	s->jr.next_cont_bit = !s->jr.next_cont_bit;
	s->jr.host_pending = false;
	return 0;
}

/* Open a block-boundary handshake: refresh SR and pulse the interrupt.  At
 * a 32-nybble boundary the SMPC stops, tells the host how much data is
 * left, and will not move on until the host continues.  A naive
 * implementation that fills all of OREG at once and pulses once at the end
 * is subtly wrong in a way games notice, because the host is allowed to read
 * only the bytes that have been written so far. */
static void host_begin_block(Smpc *s)
{
	s->sr = (uint8_t)((s->sr & ~SMPC_SR_PDL) |
	                  ((s->jr.pd_counter < 0x2) ? SMPC_SR_PDL : 0));
	s->sr = (uint8_t)((s->sr & ~0x0F) |
	                  ((s->jr.mode[0] & 3) << 0) |
	                  ((s->jr.mode[1] & 3) << 2));
	s->sr |= SMPC_SR_NPE;
	s->sr |= 0x80;
	pulse_system_manager_irq(s);

	s->ir0wx = (uint8_t)((!s->jr.next_cont_bit) << 7);
	s->ir0wa = 0xC0;
	s->jr.host_pending = true;
}

/* Interpreter step.  Returns:
 *   0  finished (ran off the end of the stream)
 *   1  suspended (a wait is outstanding; call again once the clock advances)
 *   2  aborted (vblank ended mid-report)
 */
static int report_step(Smpc *s)
{
	if (s->pending_vb)
		return 2;

	while (s->jr.pc < s->jr.prog_len) {
		const SmpcOp in = s->jr.prog[s->jr.pc];
		const uint16_t arg = in.arg;

		switch (in.op) {
		case OP_END:
			return 0;

		case OP_EAT: {
			/* Peek, then commit, so a suspension does not double-charge. */
			const int64_t need = (int64_t)arg << 32;

			if (s->clock_counter < need) {
				const int64_t missing = need - s->clock_counter;

				s->jr.wake = (int32_t)((missing + s->clock_ratio - 1) /
				                       s->clock_ratio);
				return 1;
			}
			s->clock_counter -= need;
			s->jr.pc++;
			break;
		}

		case OP_SETTHTR:
			set_th_tr(s, s->jr.cur_port, line_dec(arg >> 2), line_dec(arg));
			s->jr.pc++;
			break;

		case OP_SAMPLE:
			s->jr.work[arg] = BS;
			s->jr.pc++;
			break;

		case OP_WAIT_TL:
			/*
			 * Wait for the peripheral's TL toggle.  Bounded, because a
			 * peripheral that stops responding must not be able to hang
			 * the report: the real SMPC gives up and the host sees a
			 * short report and an NPE that then clears.  BlueRetro's
			 * peripheral-side loop has the same structure -- it polls
			 * TR and bails out on TH or on TWH_TIMEOUT.
			 */
			if ((((BS & 0x10) != 0) ? 1u : 0u) != arg) {
				if (++s->jr.tl_timeout > TL_POLL_LIMIT) {
					s->jr.tl_timeout = 0;
					s->jr.pc++;
					break;
				}
				s->jr.wake = POLL_COND;
				return 1;
			}
			s->jr.tl_timeout = 0;
			s->jr.pc++;
			break;

		case OP_NYB: {
			/* A 32-nybble boundary in the OREG write pointer is the
			 * point at which the SMPC must stop and handshake. */
			if (!s->jr.host_pending && !s->jr.owp && s->jr.pd_counter > 0) {
				host_begin_block(s);
			}
			if (s->jr.host_pending) {
				const int r = host_continue(s);
				if (r) {
					s->jr.wake = POLL_COND;
					return r;
				}
			}
			if (!s->jr.owp && s->jr.pd_counter < 0xFF)
				s->jr.pd_counter++;

			if (getenv("SMPC_PROG")) {
				static const char *const kn[] = { "CONST", "REG_HI", "REG_LO", "WORK", "WORK_OR7" };
				fprintf(stderr, "  NYB owp=%u port=%u ctrTAP=%u/%u ctrDATA=%u/%u id1=%02X id2=%02X idtap=%02X src=%s:%u w=%02X%02X%02X%02X\n",
				        s->jr.owp, s->jr.cur_port,
				        s->jr.ctr[1], s->jr.ctr_max[1],
				        s->jr.ctr[2], s->jr.ctr_max[2],
				        s->jr.id1, s->jr.id2, s->jr.id_tap,
				        (arg & 7) < 5 ? kn[arg & 7] : "?", (arg >> 3) & 0xF,
				        s->jr.work[0], s->jr.work[1], s->jr.work[2], s->jr.work[3]);
			}
			s->oreg[s->jr.owp >> 1] &=
				(uint8_t)(0x0F << ((s->jr.owp & 1) << 2));
			s->oreg[s->jr.owp >> 1] |=
				(uint8_t)((nyb_source(s, arg) & 0xF) << ((((s->jr.owp & 1) ^ 1)) << 2));
			s->jr.owp = (uint8_t)((s->jr.owp + 1) & 0x3F);
			s->jr.pc++;
			break;
		}

		case OP_REPEAT:
			/* Push: the frame index is the depth as it stands, and the
			 * depth then points one past the top frame for as long as the
			 * body is running. */
			if (s->jr.loop_depth < SMPC_LOOP_FRAMES)
				s->jr.loop_start[s->jr.loop_depth] = (uint16_t)(s->jr.pc + 1);
			s->jr.loop_depth++;
			s->jr.pc++;
			break;

		case OP_ENDREPEAT:
			/* arg selects which counter drives this loop.
			 *
			 * The test is `ctr + 1 < max`, not `ctr < max`, because this
			 * is a do-while: the body has already run once by the time
			 * control reaches here, and `ctr` starts at zero.  With a
			 * plain `ctr < max` a loop bounded by N runs N+1 times, which
			 * gave the port loop three passes over two ports and the
			 * sub-slot loop an extra empty slot behind every pad.
			 *
			 * Loops whose body increments the counter itself (the data
			 * loops used to, via STORE/LOAD) would then run short, which
			 * is why nothing inside a loop body touches its counter any
			 * more: the index for those loops is the counter, and the
			 * increment belongs here and here only. */
			if ((uint32_t)s->jr.ctr[arg] + 1u < (uint32_t)s->jr.ctr_max[arg]) {
				/* Still inside: peek at the top frame, do not pop it.
				 * Popping here left the depth one short of the frame it
				 * belonged to, so the next REPEAT overwrote the outer
				 * loop's start address and the port loop then jumped back
				 * into the sub-slot body instead of into the port loop.
				 * The report ran port 0 twice and never reached port 1. */
				s->jr.ctr[arg]++;
				if (s->jr.loop_depth > 0)
					s->jr.pc = s->jr.loop_start[s->jr.loop_depth - 1];
				else
					s->jr.pc++;
			} else {
				/* Finished: pop. */
				s->jr.ctr[arg] = 0;
				if (s->jr.loop_depth > 0)
					s->jr.loop_depth--;
				s->jr.pc++;
			}
			break;

		case OP_GOTO:
			s->jr.pc = arg;
			break;

		case OP_SET_CTRMAX: {
			const int ctr = arg & 0x03;
			const int src = (arg >> 2) & 0x03;
			unsigned v;

			switch (src) {
			case CTRMAX_ID2:
				/* Beetle's ReadCount: ((id2 & 0xF0) == 0xF0) ? 0 :
				 * (id2 & 0xF).  The all-ones id an empty sub-slot
				 * answers with therefore means *no* payload; taking
				 * the low nybble gave it 15 bytes and ran the report
				 * three times over the 64-nybble DMA window. */
				v = ((s->jr.id2 & 0xF0) == 0xF0) ? 0u
				    : (unsigned)(s->jr.id2 & 0x0F);
				break;
			case CTRMAX_IDTAP:  v = (unsigned)(s->jr.id_tap & 0x0F); break;
			default:           v = SMPC_PORT_COUNT; break;
			}
			/* A multi-tap has six connectors; anything more is a
			 * mis-decode and must not walk off the sub-device array. */
			if ((arg >> 4) & 1)
				v = (v > 6) ? 6 : v;
			s->jr.ctr_max[ctr] = (uint16_t)v;
			s->jr.ctr[ctr] = 0;
			s->jr.pc++;
			break;
		}

		case OP_IF_PORT_SKIP:
			if (s->jr.mode[arg & 0x03] & 2)
				s->jr.pc = (uint16_t)(arg >> 2);
			else
				s->jr.pc++;
			break;

		case OP_IF_CTRMAX_ZERO:
			/* A do-while runs at least once no matter what the bound
			 * says, so a loop that may legitimately execute zero times
			 * -- a tap advertising no pads, a sub-slot with no payload
			 * -- needs an explicit guard in front of it. */
			if (s->jr.ctr_max[arg & 0x0F] == 0)
				s->jr.pc = (uint16_t)(arg >> 8);
			else
				s->jr.pc++;
			break;

		case OP_IF_TAP_LE1:
			/* Beetle tests `TapCount > 1` before re-reading id2 for a
			 * sub-slot: a directly-connected pad reports TapCount 1 and
			 * keeps using the port-level id2.  is_tap alone is not the
			 * same test -- a tap with a single pad would diverge. */
			if ((s->jr.id_tap & 0x0F) <= 1)
				s->jr.pc = arg;
			else
				s->jr.pc++;
			break;

		case OP_IF_ID1:
			if (s->jr.id1 == (uint16_t)(arg & 0x0F))
				s->jr.pc = (uint16_t)(arg >> 4);
			else
				s->jr.pc++;
			break;

		/*
		 * Two separate tests, not one with a polarity argument.  Every
		 * guard around the multi-tap blocks needs a different sense, and
		 * collapsing them into one instruction with a flag is how all
		 * three of them ended up inverted at once: the device id and
		 * size got overwritten with 0xFF on a directly-connected pad,
		 * which set the data length to 15 and stalled the report.
		 */
		case OP_IF_MULTI:
			if (s->jr.is_tap)
				s->jr.pc = arg;
			else
				s->jr.pc++;
			break;

		case OP_IF_NOT_MULTI:
			if (!s->jr.is_tap)
				s->jr.pc = arg;
			else
				s->jr.pc++;
			break;

		case OP_IF_CTR_NONZERO:
			/* Jumps when the counter is *past* its first iteration, so
			 * the guarded block runs exactly once.  Used for the
			 * multi-tap header, which is emitted in front of the first
			 * sub-device only. */
			if (s->jr.ctr[arg & 0x0F] != 0)
				s->jr.pc = (uint16_t)(arg >> 8);
			else
				s->jr.pc++;
			break;

		case OP_SET_ID1:
			/*
			 * The id nybble comes back with its bits out of order,
			 * so two adjacent nybbles have to be recombined bit-pair
			 * by bit-pair rather than simply concatenated.  Getting
			 * this wrong is not a small error: it changes which arm of
			 * the device dispatch is taken, so a 3D pad would be
			 * mistaken for a first-generation digital pad and every
			 * subsequent nybble would be wrong.
			 */
			s->jr.id1 = (uint8_t)(((((s->jr.work[0] >> 3) | (s->jr.work[0] >> 2)) & 1) << 3) |
			                     ((((s->jr.work[0] >> 1) | (s->jr.work[0] >> 0)) & 1) << 2) |
			                     ((((s->jr.work[1] >> 3) | (s->jr.work[1] >> 2)) & 1) << 1) |
			                     ((((s->jr.work[1] >> 1) | (s->jr.work[1] >> 0)) & 1) << 0));
			s->jr.pc++;
			break;

		case OP_SET_ID2:
			s->jr.id2 = (uint8_t)(((s->jr.work[0] & 0x0F) << 4) |
			                     (s->jr.work[1] & 0x0F));
			/* A multi-tap identifies itself with a 0x4x header; the
			 * low nybble is the number of attached pads.  Only the
			 * port-level read may do this: the sub-slot loop re-reads
			 * id2, a sub-pad answers 0x1x, and clearing is_tap there
			 * made `IF_NOT_MULTI` skip the probe from the second slot
			 * on, dropping the report a byte out of step with the
			 * adapter's stream. */
			s->jr.is_tap = ((s->jr.id2 & 0xF0) == 0x40);
			s->jr.pc++;
			break;

		case OP_SET_ID2_TAP:
			s->jr.id2 = (uint8_t)(((s->jr.work[0] & 0x0F) << 4) |
			                     (s->jr.work[1] & 0x0F));
			s->jr.pc++;
			break;

		case OP_SET_IDTAP:
			/* work[2], not work[3]: the count is sampled with TL still
			 * low, which is the *high* nybble of the adapter's count
			 * byte -- the slot BlueRetro puts `nb_port << 4` in.  work[3]
			 * is the low nybble and is always zero, so the count came
			 * out as 0 and the sub-slot loop ran exactly once instead of
			 * once per attached pad. */
			s->jr.id_tap = (uint8_t)(((s->jr.id2 & 0x0F) << 4) |
			                         (s->jr.work[2] & 0x0F));
			s->jr.pc++;
			break;

		case OP_SET_IDTAP_DIRECT:
			s->jr.id_tap = 0xF1;
			s->jr.is_tap = false;
			s->jr.pc++;
			break;

		case OP_FORCE_MOUSE_ID:
			if (s->jr.id1 == arg)
				s->jr.id2 = 0xE3;
			s->jr.pc++;
			break;

		case OP_STORE_DATABYTE:
			s->jr.read_buffer[s->jr.ctr[CTR_DATA]] =
				(uint8_t)(((s->jr.work[0] & 0x0F) << 4) | (s->jr.work[1] & 0x0F));
			s->jr.pc++;
			break;

		case OP_LOAD_DATABYTE:
			/* Both halves, not just the high one: the emitter writes
			 * work[0] then work[1], so leaving work[1] holding whatever
			 * the last bus sample put there made every second nybble of
			 * every reported data byte come from the id probe. */
			{
				const uint8_t b = s->jr.read_buffer[s->jr.ctr[CTR_DATA]];

				s->jr.work[0] = (uint8_t)(b >> 4);
				s->jr.work[1] = (uint8_t)(b & 0x0F);
			}
			s->jr.pc++;
			break;

		case OP_NEXT_PORT:
			/* Wrap.  The increment also runs after the last port, and
			 * cur_port indexes s->port[] directly, so an unwrapped
			 * increment walks off the end of the array. */
			s->jr.cur_port = (uint8_t)((s->jr.cur_port + 1) % SMPC_PORT_COUNT);
			s->jr.ctr[CTR_DATA] = 0;
			s->jr.is_tap = false;
			s->jr.pc++;
			break;

		default:
			return 0;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Report program construction                                         */
/* ------------------------------------------------------------------ */

/*
 * Build the sequencer program for the peripheral half of an INTBACK.
 *
 * This is the translation of Mednafen's JR_* macro sequence.  The shape of
 * the exchange is:
 *
 *   - present a port status nybble (0xF1 direct, or the multi-tap header)
 *   - for each attached device, a peripheral-id nybble and a size nybble,
 *     then that many data bytes
 *   - a 0x00 terminator
 *
 * The per-port data size is not known when the program is built, because it
 * comes back over the wire from a multi-tap that has just been interrogated.
 * So the program is emitted up to that point, the interpreter runs it, and
 * the tail is appended as soon as the size is known.  The interpreter simply
 * stops when it runs off the end of the stream.
 */

static void emit_2nibble_sample(Smpc *s, int work_hi, int work_lo)
{
	emit_set(s, 1, 1);
	emit_eat(s, EAT_NYBBLE_SETTLE);
	emit_sample(s, work_hi);
	emit_set(s, 0, 1);
	emit_eat(s, EAT_NYBBLE_SETTLE);
	emit_sample(s, work_lo);
}

static void emit_nyb_eat(Smpc *s, uint8_t desc)
{
	emit(s, OP_NYB, desc);
	emit_eat(s, EAT_NYBBLE);
}

static uint16_t emit_goto_here(Smpc *s)
{
	const uint16_t pc = (uint16_t)s->jr.prog_len;

	emit(s, OP_GOTO, 0);
	return pc;
}

static void patch_goto(Smpc *s, uint16_t at, uint16_t target)
{
	s->jr.prog[at].arg = target;
}

/*
 * Read one byte from the current port: drive TR low, wait for TL low, take
 * the high nybble; drive TR high, wait for TL high, take the low nybble.
 * Self-clocking devices move TL themselves, so the SMPC has to poll rather
 * than assume a fixed delay -- this is the only place in the exchange where
 * a real pad's response time, rather than a fixed constant, sets the pace.
 */
static void emit_2nibble_read(Smpc *s, int work_hi, int work_lo)
{
	emit_set(s, 0, 0);
	emit_eat(s, EAT_NYBBLE_SETTLE);
	emit(s, OP_WAIT_TL, 0);
	emit_sample(s, work_hi);
	emit_set(s, 0, 1);
	emit_eat(s, EAT_NYBBLE_SETTLE);
	emit(s, OP_WAIT_TL, 1);
	emit_sample(s, work_lo);
}

/*
 * Emit a conditional and return the pc to patch its "else" target into.
 *
 * The convention is: the conditional *jumps over* the block when it matches,
 * and falls through into it.  Every branch target is a pc filled in after the
 * block is emitted, so the emitter never has to know the block's length in
 * advance.  Getting the sense of these backwards is silent -- the whole
 * report collapses to one arm of the test and still "completes" -- so the
 * pattern is centralised here rather than open-coded.
 */
static uint16_t emit_if_id1(Smpc *s, uint8_t expected)
{
	const uint16_t at = (uint16_t)s->jr.prog_len;

	emit(s, OP_IF_ID1, (uint16_t)(expected & 0x0F));
	return at;
}

static uint16_t emit_if_port_skip(Smpc *s)
{
	const uint16_t at = (uint16_t)s->jr.prog_len;

	emit(s, OP_IF_PORT_SKIP, CTR_PORT);
	return at;
}

/* Guards a block that runs only when a multi-tap is attached. */
static uint16_t emit_if_multi(Smpc *s)
{
	const uint16_t at = (uint16_t)s->jr.prog_len;

	emit(s, OP_IF_MULTI, 0);
	return at;
}

/* Guards a block that runs *without* a multi-tap: the direct-connection
 * case. */
static uint16_t emit_if_not_multi(Smpc *s)
{
	const uint16_t at = (uint16_t)s->jr.prog_len;

	emit(s, OP_IF_NOT_MULTI, 0);
	return at;
}

static uint16_t emit_if_ctr_nonzero(Smpc *s, int ctr)
{
	const uint16_t at = (uint16_t)s->jr.prog_len;

	emit(s, OP_IF_CTR_NONZERO, (uint16_t)(ctr & 0x0F));
	return at;
}

/* Guards a loop whose bound may be zero. */
static uint16_t emit_if_ctrmax_zero(Smpc *s, int ctr)
{
	const uint16_t at = (uint16_t)s->jr.prog_len;

	emit(s, OP_IF_CTRMAX_ZERO, (uint16_t)(ctr & 0x0F));
	return at;
}

/* Guards the sub-slot id probe: runs only for a tap with 2+ pads. */
static uint16_t emit_if_tap_le1(Smpc *s)
{
	const uint16_t at = (uint16_t)s->jr.prog_len;

	emit(s, OP_IF_TAP_LE1, 0);
	return at;
}

/* IF_ID1 packs expected in bits 0-3 and the else-target in bits 4-15. */
static void patch_if_id1(Smpc *s, uint16_t at, uint16_t target)
{
	s->jr.prog[at].arg = (uint16_t)((s->jr.prog[at].arg & 0x0F) | (target << 4));
}

/* IF_PORT_SKIP packs the counter in bits 0-1 and the target in bits 2-15. */
static void patch_if_port_skip(Smpc *s, uint16_t at, uint16_t target)
{
	s->jr.prog[at].arg = (uint16_t)((s->jr.prog[at].arg & 0x03) | (target << 2));
}

/* IF_TAP and IF_MULTI jump when a multi-tap is present; the else falls
 * through. */
static void patch_if(Smpc *s, uint16_t at, uint16_t target)
{
	s->jr.prog[at].arg = target;
}

/* OP_IF_CTR_NONZERO packs the counter in bits 0-3 and the target in 8-15. */
static void patch_if_ctr(Smpc *s, uint16_t at, uint16_t target)
{
	s->jr.prog[at].arg = (uint16_t)((s->jr.prog[at].arg & 0x00FF) | (target << 8));
}

/*
 * Emit a conditional that jumps *to* a block when it matches, with the
 * fall-through being the next case.  Every target is patched once the block
 * has been emitted.
 *
 * All the branch targets are patched after the fact, so the sense of each
 * test has to be right at emission time or the whole report silently
 * collapses to one arm and still "completes".  The device dispatch is
 * therefore laid out as a jump table rather than a chain of skip guards:
 *
 *     if id1 == 0xB  -> digital block
 *     if id1 == 0x3  -> self-clocking block
 *     if id1 == 0x5  -> self-clocking block
 *     otherwise      -> report the raw id and a zero size
 */
static void build_report_program(Smpc *s)
{
	uint16_t if_digital, if3, if5, goto_raw, goto_digital;
	uint16_t skip_port, skip_not_tap, skip_tap, skip_not_multi, skip_not_first;
	uint16_t skip_empty_tap, skip_empty_data;

	s->jr.prog_len = 0;
	s->jr.pc = 0;
	s->jr.loop_depth = 0;
	s->jr.host_pending = false;
	memset(s->jr.ctr, 0, sizeof s->jr.ctr);
	memset(s->jr.ctr_max, 0, sizeof s->jr.ctr_max);

	emit_eat(s, EAT_REPORT_PRELUDE);
	s->jr.owp = 0;

	/* Two front-panel ports, always.  The trip count has to be set
	 * explicitly: OP_REPEAT only pushes a frame, so a loop whose counter
	 * was never given a bound runs exactly once. */
	emit(s, OP_SET_CTRMAX, (uint16_t)(CTR_PORT | (CTRMAX_PORTS << 2)));
	emit(s, OP_REPEAT, 0);

	/* Per-port preamble. */
	emit_eat(s, EAT_PORT_PREAMBLE);

	/* Port mode 2 ("invalid") or 3 ("0 bytes") makes the SMPC skip the
	 * port without touching it. */
	skip_port = emit_if_port_skip(s);

	/* Learn the device generation from two id samples. */
	emit_2nibble_sample(s, 0, 1);
	emit(s, OP_SET_ID1, 0);

	/* ---- dispatch ----
	 *
	 * The arms are a forward chain: the unmatched arm is emitted first,
	 * then each recognised one, so every arm except the last has to jump
	 * past the ones behind it.  Patching those jumps to "the instruction
	 * after this arm" lands them on the *next* arm instead of on the port
	 * tail, which made an unmatched id run the raw-id arm and then the
	 * self-clocking one as well -- two reports back to back, long enough
	 * to wrap the 64-nybble write pointer and hand the host a second
	 * block-boundary interrupt. */
	if_digital = emit_if_id1(s, 0xB);
	if3 = emit_if_id1(s, 0x3);
	if5 = emit_if_id1(s, 0x5);

	/* Unrecognised: report the raw id nybble and a zero size.  Beetle
	 * writes `JRS.ID1` itself into one nybble slot, which takes its low
	 * half; the high half of a one-nybble id is always zero and turned
	 * an empty port's 0xF into 0x0, so the terminator byte came out as
	 * 00 instead of F0. */
	emit_nyb_eat(s, pack_nyb(SRC_REG_LO, 0));
	emit_nyb_eat(s, pack_nyb(SRC_CONST, 0x0));
	goto_raw = emit_goto_here(s);

	/* ---- first-generation digital pad ----
	 * No packet protocol: the pad answers the id probe with its own
	 * button nybbles, so the SMPC synthesises the whole report from four
	 * samples.  This is the path a genuine Saturn Control Pad and a Sega
	 * Mouse take. */
	patch_if_id1(s, if_digital, (uint16_t)s->jr.prog_len);

	emit_nyb_eat(s, pack_nyb(SRC_CONST, 0xF));	/* port status: 1 device  */
	emit_nyb_eat(s, pack_nyb(SRC_CONST, 0x1));	/* device count          */
	emit_nyb_eat(s, pack_nyb(SRC_CONST, 0x0));	/* peripheral id 0x00    */
	emit_nyb_eat(s, pack_nyb(SRC_CONST, 0x2));	/* 2 data bytes          */

	emit_set(s, 1, 0);
	emit_eat(s, EAT_NYBBLE_SETTLE);
	emit_sample(s, 2);
	emit_set(s, 0, 0);
	emit_eat(s, EAT_NYBBLE_SETTLE);
	emit_sample(s, 3);
	emit_eat(s, EAT_DIGITAL_GAP);

	emit_nyb_eat(s, pack_nyb(SRC_WORK, 1));
	emit_nyb_eat(s, pack_nyb(SRC_WORK, 2));
	emit_nyb_eat(s, pack_nyb(SRC_WORK, 3));
	emit_nyb_eat(s, pack_nyb(SRC_WORK_OR7, 0));
	/* Both jumps are patched once the port tail exists, below. */
	goto_digital = emit_goto_here(s);

	/* ---- self-clocking devices ----
	 * The device negotiates: the SMPC reads its id and data size, then
	 * streams that many bytes.  Patched after the jump above so that the
	 * two id1 tests land on the arm's first instruction, not on the jump. */
	patch_if_id1(s, if3, (uint16_t)s->jr.prog_len);
	patch_if_id1(s, if5, (uint16_t)s->jr.prog_len);

	emit_2nibble_read(s, 0, 1);
	emit(s, OP_SET_ID2, 0);
	/* 0x3 is forced to the mouse id: the Saturn Mouse and the Shuttle
	 * Mouse are told apart by their data format, not by anything they say
	 * on the wire. */
	emit(s, OP_FORCE_MOUSE_ID, 0x3);
	emit(s, OP_SET_CTRMAX, (uint16_t)(CTR_DATA | (CTRMAX_ID2 << 2)));

	/* A multi-tap answers with a 0x4x header whose low nybble is the
	 * number of attached pads. */
	skip_not_tap = emit_if_not_multi(s);
	emit_2nibble_read(s, 2, 3);
	emit(s, OP_SET_IDTAP, 0);
	emit(s, OP_SET_CTRMAX, (uint16_t)(CTR_TAP | (CTRMAX_IDTAP << 2) | (1 << 4)));
	patch_if(s, skip_not_tap, (uint16_t)s->jr.prog_len);

	skip_tap = emit_if_multi(s);
	emit(s, OP_SET_IDTAP_DIRECT, 0);
	/* The sub-slot loop runs once for a device wired straight to the port,
	 * just as it runs per-slot behind a multi-tap -- only the source of the
	 * count differs.  Leaving ctr_max at zero here made the loop body
	 * unreachable, so the device's own id and size nybbles were skipped and
	 * the report came out as the raw id plus a zero size. */
	emit(s, OP_SET_CTRMAX, (uint16_t)(CTR_TAP | (CTRMAX_IDTAP << 2) | (1 << 4)));
	patch_if(s, skip_tap, (uint16_t)s->jr.prog_len);

	skip_empty_tap = emit_if_ctrmax_zero(s, CTR_TAP);
	emit(s, OP_REPEAT, 0);

	/* Behind a multi-tap, each sub-device has its own id and size.
	 * Beetle guards this with `TapCount > 1`, not with is_tap. */
	skip_not_multi = emit_if_tap_le1(s);
	emit_2nibble_read(s, 0, 1);
	emit(s, OP_SET_ID2_TAP, 0);
	emit(s, OP_SET_CTRMAX, (uint16_t)(CTR_DATA | (CTRMAX_ID2 << 2)));
	patch_if(s, skip_not_multi, (uint16_t)s->jr.prog_len);

	/* The adapter header goes out in front of the first sub-device. */
	skip_not_first = emit_if_ctr_nonzero(s, CTR_TAP);
	emit_nyb_eat(s, pack_nyb(SRC_REG_HI, 2));	/* id_tap high  */
	emit_nyb_eat(s, pack_nyb(SRC_REG_LO, 2));	/* id_tap low   */
	patch_if_ctr(s, skip_not_first, (uint16_t)s->jr.prog_len);

	emit_nyb_eat(s, pack_nyb(SRC_REG_HI, 1));	/* id2 high     */
	emit_nyb_eat(s, pack_nyb(SRC_REG_LO, 1));	/* id2 low      */

	/* Both data loops are covered by one guard: an empty sub-slot
	 * reports a payload of zero, and the do-while would otherwise
	 * still emit a byte it never received. */
	skip_empty_data = emit_if_ctrmax_zero(s, CTR_DATA);
	emit(s, OP_REPEAT, 0);
	emit_2nibble_read(s, 0, 1);
	emit(s, OP_STORE_DATABYTE, 0);
	emit(s, OP_ENDREPEAT, CTR_DATA);

	emit(s, OP_REPEAT, 0);
	emit(s, OP_LOAD_DATABYTE, 0);
	emit_nyb_eat(s, pack_nyb(SRC_WORK, 0));
	emit_nyb_eat(s, pack_nyb(SRC_WORK, 1));
	emit(s, OP_ENDREPEAT, CTR_DATA);
	patch_if_ctr(s, skip_empty_data, (uint16_t)s->jr.prog_len);

	emit(s, OP_ENDREPEAT, CTR_TAP);
	patch_if_ctr(s, skip_empty_tap, (uint16_t)s->jr.prog_len);

	/* ---- end of port ----
	 * Every arm arrives here, including the two that jumped over the arms
	 * behind them.  The tail is shared because the port has to be released
	 * and advanced exactly once whichever arm ran. */
	patch_goto(s, goto_raw, (uint16_t)s->jr.prog_len);
	patch_goto(s, goto_digital, (uint16_t)s->jr.prog_len);
	emit_eat(s, EAT_PORT_TAIL);
	emit_set(s, -1, -1);

	patch_if_port_skip(s, skip_port, (uint16_t)s->jr.prog_len);

	emit(s, OP_NEXT_PORT, 0);
	emit(s, OP_ENDREPEAT, CTR_PORT);
	emit(s, OP_END, 0);

/* temporary diagnostic, appended inside build_report_program */
	if (getenv("SMPC_PROG")) {
		static const char *const nm[] = {
			"END","EAT","SETTHTR","SAMPLE","WAIT_TL","NYB","REPEAT",
			"ENDREPEAT","GOTO","SET_CTRMAX","IF_PORT_SKIP","IF_ID1",
			"IF_TAP","IF_CTR_NONZERO","SET_ID1","SET_ID2","SET_ID2_TAP",
			"SET_IDTAP","SET_IDTAP_DIRECT","FORCE_MOUSE_ID","STORE_DATABYTE",
			"LOAD_DATABYTE","NEXT_PORT","IF_MULTI","IF_NOT_MULTI",
			"IF_CTRMAX_ZERO","IF_TAP_LE1"
		};
		fprintf(stderr, "--- report program, %u ops\n", s->jr.prog_len);
		for (uint32_t i = 0; i < s->jr.prog_len; i++) {
			const SmpcOp o = s->jr.prog[i];
			fprintf(stderr, "  %3u  %-16s %u\n", i,
			        o.op < sizeof nm / sizeof nm[0] ? nm[o.op] : "?",
			        o.arg);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Command engine                                                      */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Status report                                                       */
/* ------------------------------------------------------------------ */

/*
 * OREG0..OREG15, the "status" half of an INTBACK.
 *
 *   [0]  bit7 RTC valid (0 = backup battery dead, BIOS must run the
 *        language/clock setup), bit6 reset disable
 *   [1..7] the RTC's own seven bytes
 *   [8]  cartridge code, low bits          [9] area code
 *   [10] system status 1: DOTSEL, slave SH2, master NMI, SYSRES, SNDRES
 *   [11] system status 2: CDRES
 *   [12..15] SMEM, the four bytes of SMPC internal RAM
 *
 * Bit 7 of OREG0 is the one field the hardware forces: on a console whose
 * backup battery is dead the SMPC reports 0 there and the BIOS walks the
 * user through language and clock setup.  It is a real battery measurement,
 * not a saved flag, so a replacement has to be able to say "no".
 */
static void write_status_report(Smpc *s)
{
	/*
	 * NOT pre-filled with 0xFF.
	 *
	 * MAME fills OREG16..30 and calls them "undefined"; Kronos's Yabause
	 * fills OREG0..30.  Neither Ymir, nor Beetle, nor upstream Yabause do,
	 * and Beetle -- the only implementation this project can diff against
	 * automatically -- reports 0x00 there.  Three sources against one, but
	 * the disagreement is unresolvable without hardware, so the behaviour
	 * that can be verified is kept and the conflict is recorded in
	 * docs/smpc-implementations.md rather than silently adopted.
	 */

	s->oreg[0] = (uint8_t)((s->rtc_valid ? 0x80u : 0u) |
	                       (s->reset_nmi_enabled ? 0u : 0x40u));
	memcpy(&s->oreg[1], s->rtc_raw, 7);
	s->oreg[0x8] = 0x00;		/* cartridge code; not implemented */
	s->oreg[0x9] = (uint8_t)(s->area_code & 0x0F);
	s->oreg[0xA] = (uint8_t)(0x24 |
	                        ((s->current_clock_divisor == SMPC_CLOCK_DIVISOR_28M) ? 0x40u : 0u) |
	                        (s->slave_sh2_on ? 0x10u : 0u) |
	                        0x08u |	/* master SH2 NMI pending */
	                        0x02u |	/* system reset state    */
	                        (s->sound_cpu_on ? 0x01u : 0u));
	s->oreg[0xB] = (uint8_t)((s->cd_on ? 0x40u : 0u) | 0x02u);
	memcpy(&s->oreg[0xC], s->smem, 4);
}

/* ------------------------------------------------------------------ */
/* Command execution                                                   */
/* ------------------------------------------------------------------ */

/*
 * The simple commands.  Only the clock change and the reset have any
 * duration worth modelling, and the reset is completed by the surrounding
 * system at the next frame boundary, exactly as on hardware.
 */
static void exec_simple(Smpc *s, int cmd)
{
	switch (cmd) {
	case SMPC_CMD_MSHON:
		/* Master SH-2 is always on; the ROM's handler is a no-op. */
		break;

	case SMPC_CMD_SSHON:
		if (!s->slave_sh2_on) {
			s->slave_sh2_pending = 1;
			if (s->env.request_ehl_exit)
				s->env.request_ehl_exit(s->env.ctx);
		}
		break;

	case SMPC_CMD_SSHOFF:
		if (s->slave_sh2_on) {
			s->slave_sh2_pending = -1;
			if (s->env.request_ehl_exit)
				s->env.request_ehl_exit(s->env.ctx);
		}
		break;

	case SMPC_CMD_SNDON:
		if (!s->sound_cpu_on) {
			/* Powering the sound CPU on resets the 68K; games rely on
			 * that to know its RAM is clear. */
			if (s->env.sound_reset)
				s->env.sound_reset(s->env.ctx);
			s->sound_cpu_on = true;
		}
		break;

	case SMPC_CMD_SNDOFF:
		if (s->sound_cpu_on) {
			if (s->env.sound_reset)
				s->env.sound_reset(s->env.ctx);
			s->sound_cpu_on = false;
		}
		break;

	case SMPC_CMD_CDON:
		s->cd_on = true;
		break;

	case SMPC_CMD_CDOFF:
		s->cd_on = false;
		break;

	case SMPC_CMD_NMIREQ:
		raise_master_nmi(s);
		break;

	case SMPC_CMD_RESENAB:
		s->reset_nmi_enabled = true;
		break;

	case SMPC_CMD_RESDISA:
		s->reset_nmi_enabled = false;
		break;

	case SMPC_CMD_SETTIME:
		s->rtc_clock_accum = 0;	/* setting the time resets sub-second */
		s->rtc_valid = true;
		memcpy(s->rtc_raw, s->ireg, 7);
		break;

	case SMPC_CMD_SETSMEM:
		memcpy(s->smem, s->ireg, 4);
		break;

	/* Documented in the ROM's dispatch table but not in any emulator's
	 * command set.  They touch the NetLink, which this core does not
	 * implement, so they retire without effect. */
	case SMPC_CMD_NETLINKON:
	case SMPC_CMD_NETLINKOFF:
		break;

	/* Undocumented copy-protection commands (0x1E / 0x1F).  They drive
	 * AD1/SMR0 on the real chip; modelled as a no-op that still retires
	 * cleanly so a protected disc does not hang the bus. */
	case SMPC_CMD_SEC_GETSEED:
	case SMPC_CMD_SEC_VERIFY:
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Main sequencer                                                      */
/* ------------------------------------------------------------------ */

/*
 * The outer state machine, in the same shape as Mednafen's: a small number
 * of phases with fallthrough, rather than one function per command, because
 * the phases have to be able to suspend for an arbitrary number of clocks
 * and resume.
 */
enum
{
	ST_WAIT_PENDING = 0,
	ST_VBLANK,
	ST_COMMAND_LATENCY,
	ST_COMMAND,
	ST_SETTIME_WAIT,
	ST_SETSMEM_WAIT,
	ST_CKCHG_WAIT,
	ST_SYSRES_WAIT,
	ST_INTBACK_STATUS_WAIT,
	ST_INTBACK_STATUS_DONE,
	ST_INTBACK_ACK_B,
	ST_INTBACK_ACK_C,
	ST_INTBACK_WAIT_NOT_VB,
	ST_INTBACK_OPT,
	ST_INTBACK_REPORT,
	ST_INTBACK_FINISH,
	ST_ABORT,
	ST_DONE
};

/* Consume `clocks` SMPC clocks or suspend into `next_phase`.  The
 * parameter is not called `phase` -- macro substitution is token based and
 * would rewrite every s->phase in the body. */
/*
 * Consume `clocks` SMPC clocks, or suspend.
 *
 * clock_counter is a 32.32 fixed-point count of SMPC clocks while the delays
 * below are whole SMPC clocks, so the comparison and the subtraction have to
 * scale.
 *
 * Two details that are easy to get wrong and were:
 *
 *  - The escape is a goto, not a break.  `break` would only leave the
 *    do-while this is wrapped in, and the rest of the case would then run
 *    with a phase that had already moved on.
 *
 *  - The resume phase must be *this* case, never the case it is waiting
 *    for.  Resuming into the target skips everything between the wait and
 *    the phase assignment -- which is how SETTIME and SETSMEM ended up
 *    dropping their writes, and how the vblank housekeeping ended up
 *    running one time too few.  Each call site therefore re-executes its own
 *    delay on resume, which is correct: the clock advanced in the meantime,
 *    so the re-run succeeds.
 *
 * The parameter is not called `phase` -- macro substitution is token based
 * and would rewrite every s->phase in the body.
 */
#define EAT_THEN(resume_phase, clocks)                                        \
	do {                                                                 \
		const int64_t need_ = (int64_t)(clocks) << 32;            \
		if (s->clock_counter < need_) {                            \
			/* Wake after however much of the delay is still        \
			 * outstanding -- not just the overshoot.  Mednafen   \
			 * gets this for free by debiting first and then      \
			 * measuring the negative balance; a peek-then-commit \
			 * has to measure the shortfall explicitly. */        \
			const int64_t missing = need_ - s->clock_counter;    \
			s->phase = (resume_phase);                          \
			next_ts = (int32_t)((missing + s->clock_ratio - 1) / \
			                    s->clock_ratio) + ts;          \
			if (next_ts <= ts)                                  \
				next_ts = ts + 1;                           \
			breakout = true;                                     \
			goto suspend;                                        \
		}                                                            \
		s->clock_counter -= need_;                                 \
	} while (0)

/*
 * Per-vblank housekeeping, done on the rising edge of the vblank signal.
 *
 * The SMPC takes its timing reference from the VSYNC line, on both edges, so
 * this is where the reset button is debounced and the RTC second is
 * counted.  The reset button needs three consecutive vblanks held before it
 * will raise an NMI, and only if RESENAB has been issued -- the BIOS
 * deliberately holds the reset masked during its own start-up so a bouncing
 * switch cannot wedge the machine.
 */
static void do_vblank_housekeeping(Smpc *s)
{
	s->sr &= (uint8_t)~SMPC_SR_RESB;

	if (s->reset_button) {
		s->sr |= SMPC_SR_RESB;
		if (s->reset_button_count >= 0) {
			s->reset_button_count++;
			if (s->reset_button_count >= 3) {
				s->reset_button_count = 3;
				if (s->reset_nmi_enabled) {
					raise_master_nmi(s);
					s->reset_button_count = -1;
				}
			}
		}
	} else {
		s->reset_button_count = 0;
	}

	/* rtc_clock_accum is fed the same 32.32 fixed-point `clocks` value as
	 * clock_counter, so the one-second threshold has to carry the << 32
	 * too.  Without it the drain loop runs 4 294 967 296 times per RTC
	 * second, and the first vblank housekeeping after a long stretch with
	 * no housekeeping -- the seven frames of a clock change, where
	 * ST_CKCHG_WAIT consumes pending_vb itself -- never returns. */
	while (s->rtc_clock_accum >= ((uint64_t)SMPC_CLOCK_HZ << 32)) {
		rtc_inc_second((SmpcRtc *)&s->rtc_raw[0]);
		s->rtc_clock_accum -= (uint64_t)SMPC_CLOCK_HZ << 32;
	}
}

/*
 * The clock-change command stalls the console for roughly three and a half
 * frames while the slave SH-2, the sound CPU, the VDPs and the SCU are reset
 * and the new clock divisor latches.  Region-switching titles depend on
 * that stall being there, so it is modelled rather than skipped: the divisor
 * is latched at the following frame boundary, exactly as in hardware.
 */
static void begin_clock_change(Smpc *s, int to_352)
{
	if (s->slave_sh2_on) {
		s->slave_sh2_pending = -1;
		if (s->env.request_ehl_exit)
			s->env.request_ehl_exit(s->env.ctx);
	}
	s->sound_cpu_on = false;
	if (s->env.sound_reset)
		s->env.sound_reset(s->env.ctx);
	if (s->env.vdp_reset)
		s->env.vdp_reset(s->env.ctx);
	if (s->env.scu_reset)
		s->env.scu_reset(s->env.ctx);
	s->pending_clock_divisor = to_352 ? SMPC_CLOCK_DIVISOR_28M : SMPC_CLOCK_DIVISOR_26M;
}

int32_t smpc_run(Smpc *s, int32_t ts)
{
	int64_t clocks;
	int32_t next_ts = ts;
	bool breakout = false;

	if (ts < s->last_ts)
		clocks = 0;
	else {
		clocks = (int64_t)(ts - s->last_ts) * s->clock_ratio;
		s->last_ts = ts;
	}

	s->clock_counter += clocks;
	s->rtc_clock_accum += (uint64_t)clocks;
	s->jr.time_counter += clocks;

	for (;;) {
		switch (s->phase) {

		case ST_WAIT_PENDING:
			if (s->pending_command < 0 && !s->pending_vb) {
				/* Idle.  Sleep off whatever credit has accumulated
				 * rather than spinning: the host will write COMREG
				 * and wake us, and the harness steps the clock for
				 * us either way. */
				const int64_t credit = s->clock_counter;

				next_ts = (int32_t)((credit + s->clock_ratio - 1) /
				                    s->clock_ratio) + ts;
				breakout = true;
				break;
			}
			if (s->pending_vb && s->pending_command < 0) {
				s->pending_vb = false;
				/* Work out how long is left of the "optimised read"
				 * window, measured from the start of vblank. */
				if (s->jr.opt_read_time) {
					const int64_t elapsed = (s->jr.time_counter >> 32) - s->jr.opt_read_time;
					s->jr.opt_wait_until_time = (int32_t)(elapsed > 0 ? 0 : elapsed);
				} else {
					s->jr.opt_wait_until_time = 0;
				}
				s->jr.time_counter = 0;
				s->phase = ST_VBLANK;
				break;
			}
			s->phase = ST_COMMAND_LATENCY;
			break;

		case ST_VBLANK:
			EAT_THEN(ST_VBLANK, EAT_VBLANK_HOUSEKEEPING);
			do_vblank_housekeeping(s);
			s->phase = ST_WAIT_PENDING;
			break;

		case ST_COMMAND_LATENCY:
			/* Every command, whatever it is, takes this long to take
			 * effect.  Software that writes COMREG and immediately reads
			 * SF depends on it. */
			EAT_THEN(ST_COMMAND_LATENCY, EAT_COMMAND_LATENCY);
			s->executing_command = s->pending_command;
			s->pending_command = -1;
			if (s->executing_command >= 0 && s->executing_command < 0x20)
				s->oreg[0x1F] = (uint8_t)s->executing_command;
			s->phase = ST_COMMAND;
			break;

		case ST_COMMAND:
			switch (s->executing_command) {
			case SMPC_CMD_INTBACK:
				s->sr &= (uint8_t)~SMPC_SR_NPE;
				/* IREG0 bit 0 asks for the status report, IREG1 bit 3
				 * for the peripheral report.  Neither means the SMPC
				 * has nothing to do and retires at once. */
				s->phase = ((s->ireg[0] & 0x0F) != 0) ? ST_INTBACK_STATUS_WAIT
				                                      : ST_INTBACK_ACK_B;
				break;

			case SMPC_CMD_SETTIME:
				s->phase = ST_SETTIME_WAIT;
				s->jr.wake = EAT_SETTIME;
				breakout = true;
				break;

			case SMPC_CMD_SETSMEM:
				s->phase = ST_SETSMEM_WAIT;
				s->jr.wake = EAT_SETSMEM;
				breakout = true;
				break;

			case SMPC_CMD_CKCHG352:
			case SMPC_CMD_CKCHG320:
				begin_clock_change(s, s->executing_command == SMPC_CMD_CKCHG352);
				s->phase = ST_CKCHG_WAIT;
				breakout = true;
				break;

	case SMPC_CMD_SYSRES:
		/* The command does not retire when it is accepted; it retires at
		 * the frame boundary, when the reset is actually applied.  SF
		 * therefore stays set across the whole wait, and software that
		 * polls SF instead of waiting a frame sees the command still in
		 * flight -- Beetle times the BIOS's poll out for exactly that
		 * reason. */
		s->reset_pending = true;
		s->phase = ST_SYSRES_WAIT;
		breakout = true;
		break;

	default:
		exec_simple(s, s->executing_command);
		s->phase = ST_DONE;
		break;
	}
	break;

		case ST_SETTIME_WAIT:
			EAT_THEN(ST_SETTIME_WAIT, EAT_SETTIME);
			exec_simple(s, SMPC_CMD_SETTIME);
			s->phase = ST_DONE;
			break;

		case ST_SETSMEM_WAIT:
			EAT_THEN(ST_SETSMEM_WAIT, EAT_SETSMEM);
			exec_simple(s, SMPC_CMD_SETSMEM);
			s->phase = ST_DONE;
			break;

		case ST_CKCHG_WAIT:
			/*
			 * Wait out the reset in vblank units: three and a half
			 * frames, then the divisor latches at a frame boundary, then
			 * the NMI.  The poll granularity is the same 1000 master
			 * clocks the rest of the sequencer uses -- spinning at one
			 * clock would be faithful to nothing and cost a frame of
			 * emulation time per frame of console time.
			 */
			if (s->pending_vb) {
				s->pending_vb = false;
				s->jr.ckchg_vb++;
				if (s->jr.ckchg_vb == 3) {
					/* The new clock takes effect at the frame boundary,
					 * not the instant the command completes.  This is the
					 * only place pending_clock_divisor is consumed: it has
					 * a single owner, because an earlier consumer left
					 * current_clock_divisor reading 0 and clock_ratio with
					 * it, which the sleep calculation then divided by. */
					if (s->pending_clock_divisor > 0) {
						s->current_clock_divisor = s->pending_clock_divisor;
						s->pending_clock_divisor = 0;
						s->clock_ratio =
							(((int64_t)1 << 32) * SMPC_CLOCK_HZ *
							 s->current_clock_divisor) / s->master_clock;
					}
				} else if (s->jr.ckchg_vb >= 7) {
					raise_master_nmi(s);
					s->phase = ST_DONE;
				}
				break;
			}
			next_ts = ts + POLL_COND;
			breakout = true;
			break;

		case ST_SYSRES_WAIT:
			/* Held here, not retired: smpc_start_frame() applies the
			 * reset and clears reset_pending, which is what lets SF
			 * fall.  Until then the host's SF poll keeps spinning. */
			if (!s->reset_pending) {
				s->phase = ST_DONE;
				break;
			}
			next_ts = ts + POLL_COND;
			breakout = true;
			break;

		case ST_INTBACK_STATUS_WAIT:
			/* The status phase costs a fixed 952 SMPC clocks (238 us)
			 * before the data appears.  Skipping that delay is not
			 * cosmetic: the SH-2 has already been told to expect
			 * nothing, and the BIOS reads OREG from its handler. */
			EAT_THEN(ST_INTBACK_STATUS_WAIT, EAT_STATUS_REPORT);
			s->phase = ST_INTBACK_STATUS_DONE;
			break;

		case ST_INTBACK_STATUS_DONE:
			write_status_report(s);
			if (s->ireg[1] & 0x08)
				s->sr |= SMPC_SR_NPE;
			/* Bit 7 of SR distinguishes a status report (0) from a
			 * peripheral report (1); the low nybble carries the two
			 * port modes. */
			s->sr &= (uint8_t)~0x80;
			s->sr |= 0x0F;
			pulse_system_manager_irq(s);
			s->phase = ST_INTBACK_ACK_B;
			break;

		case ST_INTBACK_ACK_B:
			if (s->ireg[1] & 0x08) {
				s->phase = ST_INTBACK_ACK_C;
				break;
			}
			s->phase = ST_DONE;
			break;

		case ST_INTBACK_ACK_C:
			/* If the status report was already delivered, wait for the
			 * host to acknowledge it before starting the peripheral
			 * half. */
			s->jr.next_cont_bit = true;
			if (s->sr & SMPC_SR_NPE) {
				/* Arm the watch mask so a host write to IREG0 wakes
				 * the sequencer, but not one that leaves both
				 * handshake bits alone. */
				s->ir0wx = (uint8_t)((!s->jr.next_cont_bit) << 7);
				s->ir0wa = 0xC0;
				if (((bool)(s->ireg[0] & 0x80)) != s->jr.next_cont_bit ||
				    (s->ireg[0] & 0x40)) {
					next_ts = ts + POLL_COND;
					breakout = true;
					break;
				}
				if (s->ireg[0] & 0x40) {
					s->ir0wa = 0;
					s->phase = ST_ABORT;
					break;
				}
				s->ir0wa = 0;
				s->jr.next_cont_bit = !s->jr.next_cont_bit;
			}
			s->phase = ST_INTBACK_WAIT_NOT_VB;
			break;

		case ST_INTBACK_WAIT_NOT_VB:
			/*
			 * Wait for the *end* of vblank before reading the pads.
			 * The report itself is produced outside vblank, which is
			 * what lets a title spend vblank copying the status report
			 * and still have the peripheral report arrive after VBlank
			 * OUT -- Virtua Racing depends on exactly that split, and
			 * Discworld depends on the timeout when it is not met.
			 *
			 * If vblank has not started, the condition is already true
			 * and the SMPC proceeds immediately.
			 */
			if (!s->vb) {
				s->phase = ST_INTBACK_OPT;
				break;
			}
			next_ts = ts + POLL_COND;
			breakout = true;
			break;

		case ST_INTBACK_OPT:
			s->jr.pd_counter = 0;
			s->jr.time_opt_en = ((s->ireg[1] & 0x02) == 0);
			/*
			 * The port mode that SR echoes back comes from IREG0
			 * bits 4-5, not IREG1.  MAME, Kronos's Yabause and
			 * 5thPlanet all read IREG0 >> 4 here; only Beetle,
			 * Ymir and upstream Yabause read IREG1.  Three
			 * independent implementations against one, and MAME's
			 * driver is the oldest of the three.  The SMPC manual
			 * says SH2CMD1, so this is flagged rather than settled --
			 * see docs/smpc-implementations.md.
			 */
			s->jr.mode[0] = (uint8_t)((s->ireg[0] >> 4) & 3);
			s->jr.mode[1] = (uint8_t)(s->jr.mode[0] & 3);
			s->jr.opt_read_time = 0;
			{
				const int64_t left = s->jr.opt_wait_until_time - (s->jr.time_counter >> 32);
				s->jr.opt_eat_time = (int32_t)(left > 0 ? 0 : left);
			}
			s->jr.opt_wait_until_time = 0;
			s->jr.cur_port = 0;
			s->phase = ST_INTBACK_REPORT;
			build_report_program(s);
			break;

		case ST_INTBACK_REPORT: {
			const int r = report_step(s);

			if (r == 2) {
				s->phase = ST_ABORT;
				break;
			}
			if (r == 1) {
				next_ts = ts + s->jr.wake;
				breakout = true;
				break;
			}
			s->sr &= (uint8_t)~SMPC_SR_NPE;
			s->sr = (uint8_t)((s->sr & ~0x0F) |
			                  ((s->jr.mode[0] & 3) << 0) |
			                  ((s->jr.mode[1] & 3) << 2));
			s->sr = (uint8_t)((s->sr & ~SMPC_SR_PDL) |
			                  ((s->jr.pd_counter < 0x2) ? SMPC_SR_PDL : 0));
			s->sr |= 0x80;
			pulse_system_manager_irq(s);
			if (s->jr.time_opt_en)
				s->jr.opt_read_time = (int32_t)(s->jr.time_counter >> 32);
			s->phase = ST_DONE;
			break;
		}

		case ST_ABORT:
			/* Vblank ended mid-report.  The SMPC gives up, clears SF,
			 * and leaves OREG half-written; the host is expected to
			 * re-request on the next frame.  The delay is a measured
			 * lower bound, not an exact figure. */
			EAT_THEN(ST_ABORT, EAT_ABORT);
			s->ir0wa = 0;
			s->phase = ST_DONE;
			break;

		case ST_DONE:
			s->sf = false;
			s->phase = ST_WAIT_PENDING;
			break;

		default:
			s->phase = ST_WAIT_PENDING;
			break;
		}

		if (breakout)
			break;
	}

suspend:
	if (next_ts <= ts)
		next_ts = ts + 1;
	return next_ts;
}

/* ------------------------------------------------------------------ */
/* Bus                                                                 */
/* ------------------------------------------------------------------ */

/*
 * The register index is the six-bit value on the SMPC's own address pins,
 * which are the SH-2's A2..A7 -- the caller is responsible for that shift,
 * since it is bus wiring rather than SMPC behaviour.  The SMPC mirrors its
 * window: 0x40..0x3F aliases, and only odd byte addresses decode.
 *
 * A read of anything that is not a register returns the last value written
 * to the port.  The SMPC's data bus is open, and software relies on this:
 * bit 7 of a PDR read and bits 7..1 of an SF read come from it, and games
 * have been seen to check them.
 */
uint8_t smpc_read(Smpc *s, int32_t ts, uint8_t addr)
{
	uint8_t ret = s->bus_buffer;
	const uint8_t a = (uint8_t)(addr & 0x3F);

	(void)ts;

	if (a >= 0x10 && a <= 0x2F) {
		ret = s->oreg[a - 0x10];
	} else if (a == 0x30) {
		ret = s->sr;
	} else if (a == 0x31) {
		ret = (uint8_t)((ret & ~0x01) | (s->sf ? 1u : 0u));
	} else if (a == 0x3A) {
		ret = (uint8_t)((ret & 0x80) | s->port[0].io_state);
	} else if (a == 0x3B) {
		ret = (uint8_t)((ret & 0x80) | s->port[1].io_state);
	}
	return ret;
}

void smpc_write(Smpc *s, int32_t ts, uint8_t addr, uint8_t value)
{
	const uint8_t a = (uint8_t)(addr & 0x3F);
	bool wake = false;

	s->bus_buffer = value;

	switch (a) {
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x03:
	case 0x04:
	case 0x05:
	case 0x06:
		/* IREG0 also carries the INTBACK continue/break handshake, so a
		 * write that toggles either bit has to wake the report engine.
		 * Comparing against a watch mask is what stops every unrelated
		 * IREG0 write from disturbing an in-flight exchange. */
		if (a == 0x00 && ((value ^ s->ir0wx) & s->ir0wa))
			wake = true;
		s->ireg[a] = value;
		break;

	case 0x0F:
		s->pending_command = value;
		wake = true;
		break;

	case 0x31:
		/*
		 * SF is read-modify-write, not a byte store: writing 0 clears
		 * the busy flag, writing 1 leaves it.  That is a property of the
		 * HMCS400, whose interrupt-control registers are only reachable
		 * through the bit-modification instructions (abrasive's
		 * HARDWARE.md), and of Kronos's Yabause, which models it as
		 * `SF &= val`.  The host relies on it to retire a command
		 * without waiting.
		 */
		if (value == 0) {
			s->sf = false;
		} else {
			/* Any write starts a command. */
			s->sf = true;
		}
		break;

	/* A host write always lands in the direct-mode set, whether or not
	 * IOSEL currently selects it. */
	case 0x3A:
		s->port[0].data_out[1] = (uint8_t)(value & 0x7F);
		update_io_bus(s, 0);
		break;

	case 0x3B:
		s->port[1].data_out[1] = (uint8_t)(value & 0x7F);
		update_io_bus(s, 1);
		break;

	case 0x3C:
		s->port[0].data_dir[1] = (uint8_t)(value & 0x7F);
		update_io_bus(s, 0);
		break;

	case 0x3D:
		s->port[1].data_dir[1] = (uint8_t)(value & 0x7F);
		update_io_bus(s, 1);
		break;

	case 0x3E:
		/* IOSEL: bit 0/1 switch each port between the SMPC's own
		 * peripheral interface and direct SH-2 access.  The register
		 * sets are independent, so both ports have to be re-driven
		 * after the switch. */
		s->port[0].direct_mode_en = (value & 0x01) != 0;
		s->port[1].direct_mode_en = (value & 0x02) != 0;
		update_io_bus(s, 0);
		update_io_bus(s, 1);
		break;

	case 0x3F:
		s->port[0].ex_latch_en = (value & 0x01) != 0;
		s->port[1].ex_latch_en = (value & 0x02) != 0;
		update_io_bus(s, 0);
		update_io_bus(s, 1);
		break;

	default:
		break;
	}

	if (wake) {
		const int32_t next = smpc_run(s, ts);
		(void)next;
	}
}

void smpc_set_vbvs(Smpc *s, int32_t ts, bool vb, bool vsync)
{
	(void)ts;

	if (s->vb != vb) {
		if (vb)
			s->pending_vb = true;
		smpc_run(s, ts);
	}
	s->vb = vb;
	s->vsync = vsync;
}

void smpc_start_frame(Smpc *s)
{
	if (s->reset_pending) {
		/* The system performs the reset at the frame boundary, not the
		 * SMPC; OREG31 keeps reporting SYSRES so the host can tell the
		 * command completed.  The reset is a real one, so it takes the
		 * SMPC-visible state with it as well: a command written in the
		 * window between the SYSRES being issued and the boundary is
		 * dropped (Beetle's SS_Reset reaches SMPC_Reset, which zeroes
		 * PendingCommand, OREG and BusBuffer), and an SF read afterwards
		 * must come back as 0 rather than echoing the last COMREG byte. */
		if (s->env.vdp_reset)
			s->env.vdp_reset(s->env.ctx);
		if (s->env.sound_reset)
			s->env.sound_reset(s->env.ctx);
		s->reset_pending = false;
		s->pending_command = -1;
		memset(s->oreg, 0, sizeof(s->oreg));
		s->bus_buffer = 0;
		s->sf = false;
		s->oreg[0x1F] = SMPC_CMD_SYSRES;
	}

	/* pending_clock_divisor is deliberately NOT applied here.  It belongs
	 * to the CKCHG state machine in smpc_run(), which applies it at its
	 * third vblank; taking it here as well consumed it before that branch
	 * ran, left current_clock_divisor at 0 and clock_ratio with it, and the
	 * idle-path sleep calculation divided by zero.  Recomputing the ratio
	 * from the current divisor is still correct either way. */
	s->clock_ratio = (((int64_t)1 << 32) * SMPC_CLOCK_HZ * s->current_clock_divisor) /
	                  s->master_clock;
}

void smpc_poll_system(Smpc *s)
{
	if (s->slave_sh2_pending) {
		s->slave_sh2_on = s->slave_sh2_pending > 0;
		if (s->env.sh2_active)
			s->env.sh2_active(s->env.ctx, 1, s->slave_sh2_on);
		s->slave_sh2_pending = 0;
	}
}

bool smpc_reset_pending(const Smpc *s) { return s->reset_pending; }

/* Exposed for the testbench: which command, and how far through the
 * sequencer the core currently is.  Useful when a golden diff fails and the
 * question is "is it behind, or is it somewhere else entirely". */
int smpc_debug_command(const Smpc *s) { return s->executing_command; }
int smpc_debug_phase(const Smpc *s) { return s->phase; }
int64_t smpc_debug_clock_counter(const Smpc *s) { return s->clock_counter; }

uint8_t smpc_read_reg(Smpc *s, uint8_t a)
{
	const uint8_t r = (uint8_t)(a & 0x3F);

	if (r >= 0x10 && r <= 0x2F)
		return s->oreg[r - 0x10];
	if (r == 0x30)
		return s->sr;
	if (r == 0x31)
		return s->sf ? 1u : 0u;
	if (r == 0x3A)
		return s->port[0].io_state;
	if (r == 0x3B)
		return s->port[1].io_state;
	return s->bus_buffer;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

size_t smpc_sizeof(void) { return sizeof(struct Smpc); }
size_t smpc_alignof(void) { return _Alignof(struct Smpc); }

void smpc_init(Smpc *s, const SmpcEnv *env, uint8_t area_code,
               int32_t master_clock, bool block_sound_cpu_control)
{
	memset(s, 0, sizeof(*s));
	s->env = *env;
	s->area_code = (uint8_t)(area_code & 0x0F);
	s->master_clock = master_clock;
	s->block_sound_cpu_control = block_sound_cpu_control;
	s->pending_command = -1;
	s->executing_command = -1;
	s->phase = ST_WAIT_PENDING;

	for (unsigned i = 0; i < 6; i++)
		s->devices[i] = smpc_iodev_create(SMPC_DEV_NONE);

	for (unsigned p = 0; p < 2; p++) {
		s->port[p].tap = smpc_multitap_create();
	}

	/* A reset is deliberately *not* performed here.  Mednafen splits
	 * SMPC_Init (device construction) from SMPC_Reset (state), and the
	 * harness calls both; doing the reset inside init as well would
	 * double every reset side effect and, more importantly, would hide
	 * the difference between "powered up" and "reset while running". */
}

void smpc_kill(Smpc *s)
{
	for (unsigned i = 0; i < 6; i++) {
		if (s->devices[i] && s->owns_device[i])
			smpc_iodev_free(s->devices[i]);
		s->devices[i] = NULL;
	}
	for (unsigned p = 0; p < 2; p++) {
		smpc_multitap_free(s->port[p].tap);
		s->port[p].tap = NULL;
	}
}

void smpc_reset(Smpc *s, bool powering_up)
{
	s->slave_sh2_pending = 0;
	s->slave_sh2_on = false;
	if (s->env.sh2_active)
		s->env.sh2_active(s->env.ctx, 1, s->slave_sh2_on);
	s->sound_cpu_on = false;
	/* The sound CPU comes out of a power-on reset held, so this is a real
	 * reset pulse on the SCSP's 68K, not just a bookkeeping flag. */
	if (s->env.sound_reset)
		s->env.sound_reset(s->env.ctx);
	s->cd_on = true;

	s->reset_button_count = 0;
	s->reset_nmi_enabled = false;
	s->jr.host_pending = false;
	s->jr.is_tap = false;

	assert_master_nmi(s);

	memset(s->ireg, 0, sizeof s->ireg);
	memset(s->oreg, 0, sizeof s->oreg);
	s->pending_command = -1;
	s->executing_command = -1;
	s->sr = 0x00;
	s->sf = false;
	s->bus_buffer = 0x00;

	for (unsigned port = 0; port < 2; port++) {
		for (unsigned sel = 0; sel < 2; sel++) {
			s->port[port].data_out[sel] = 0;
			s->port[port].data_dir[sel] = 0;
		}
		s->port[port].direct_mode_en = false;
		s->port[port].ex_latch_en = false;
		update_io_bus(s, port);
		if (powering_up) {
			if (s->devices[port])
				smpc_iodev_power(s->devices[port]);
			update_io_bus(s, port);
		}
	}

	s->reset_pending = false;
	s->pending_clock_divisor = 0;
	s->current_clock_divisor = SMPC_CLOCK_DIVISOR_26M;
	s->clock_ratio = (((int64_t)1 << 32) * SMPC_CLOCK_HZ * s->current_clock_divisor) /
	                  s->master_clock;

	s->phase = ST_WAIT_PENDING;
	s->pending_vb = false;
	s->clock_counter = 0;
	s->ir0wx = 0;
	s->ir0wa = 0;
	memset(&s->jr, 0, sizeof s->jr);
	s->jr.host_pending = false;
}

/* ------------------------------------------------------------------ */
/* Peripheral configuration                                            */
/* ------------------------------------------------------------------ */

void smpc_set_peripheral(Smpc *s, unsigned port, SmpcDevType type)
{
	if (port >= 6)
		return;
	if (s->owns_device[port] && s->devices[port])
		smpc_iodev_free(s->devices[port]);
	s->devices[port] = smpc_iodev_create(type);
	s->owns_device[port] = true;
}

void smpc_set_peripheral_dev(Smpc *s, unsigned port, SmpcIoDev *dev, bool take_ownership)
{
	if (port >= 6)
		return;
	if (s->owns_device[port] && s->devices[port])
		smpc_iodev_free(s->devices[port]);
	s->devices[port] = dev;
	s->owns_device[port] = take_ownership;
}

void smpc_set_multitap(Smpc *s, unsigned port, bool enabled)
{
	struct SmpcPort *p;

	if (port >= 2)
		return;
	p = &s->port[port];
	p->tap_enabled = enabled;

	/* A multi-tap takes over the port and is handed the next virtual pads
	 * as its sub-connectors.  Beetle's MapPorts() walks a single cursor
	 * over VirtualPorts: the tap on port 0 consumes slots 0..5, so sub 0
	 * is the pad configured for input port 0 -- not input port 1.  The
	 * old `port + 1 + i` skipped it, so every sub-slot reported the pad
	 * one place to its right and the third came back empty.
	 *
	 * The cursor also matters for the *other* physical port: once the
	 * tap has consumed 0..5 there is no slot 6, so port 1 answers as if
	 * nothing were plugged in, exactly as Beetle's NULL VirtualPorts[6]
	 * does.  That is handled by port_device_index() below. */
	if (enabled) {
		for (unsigned i = 0; i < 6; i++) {
			const unsigned idx = port + i;

			smpc_multitap_set_sub(p->tap, i,
			                      idx < 6 ? s->devices[idx] : NULL);
		}
	}
	update_io_bus(s, port);
}

void smpc_update_input(Smpc *s, unsigned port, const uint8_t *data)
{
	if (port < 6 && s->devices[port])
		smpc_iodev_update_input(s->devices[port], data, 0);
}
