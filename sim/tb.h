/*
 * tb.h -- shared testbench driver.
 *
 * The scenarios live here and are compiled twice: once against the golden
 * model (Beetle's smpc.c, see sim/golden/) and once against our own core.
 * Both runs therefore receive literally the same stimulus at the SH-2 bus
 * level, and their canonical traces are diffed.  Nothing in a scenario may
 * reach around the SmpcBackend vtable -- that is what makes the diff
 * meaningful.
 */
#ifndef SMPC_TB_H
#define SMPC_TB_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Register addresses as seen by the SH-2, i.e. byte addresses in
 * 0x0100'0000..0x017F'FFFF.  Only odd addresses decode.  The SMPC sees
 * (addr & 0x7F) >> 1. */
enum
{
	TB_IREG0 = 0x01,
	TB_IREG1 = 0x03,
	TB_IREG2 = 0x05,
	TB_IREG3 = 0x07,
	TB_IREG4 = 0x09,
	TB_IREG5 = 0x0B,
	TB_IREG6 = 0x0D,
	TB_COMREG = 0x1F,
	TB_OREG0 = 0x21,
	TB_OREG31 = 0x5F,
	TB_SR = 0x61,
	TB_SF = 0x63,
	TB_PDR1 = 0x75,
	TB_PDR2 = 0x77,
	TB_DDR1 = 0x79,
	TB_DDR2 = 0x7B,
	TB_IOSEL = 0x7D,
	TB_EXLE = 0x7F
};

/* Status register bits. */
enum
{
	TB_SR_RESB = 0x10,	/* reset button held   */
	TB_SR_NPE  = 0x20,	/* peripheral data remains */
	TB_SR_PDL  = 0x40	/* first data block   */
};

enum
{
	TB_CLOCK_NTSC_352 = 0,	/* 28.636364 MHz, DOTSEL=1 */
	TB_CLOCK_NTSC_320 = 1,	/* 26 MHz,       DOTSEL=0 */
	TB_CLOCK_PAL_352  = 2,	/* 28.4375 MHz,  DOTSEL=1 */
	TB_CLOCK_PAL_320  = 3
};

typedef struct
{
	const char *name;

	void     (*init)(uint8_t area, int32_t master_clock, bool block_snd);
	void     (*reset)(bool powering_up);
	void     (*start_frame)(void);
	int32_t  (*update)(int32_t ts);
	void     (*set_vbvs)(int32_t ts, bool vb, bool vsync);
	void     (*write)(int32_t ts, uint8_t a, uint8_t v);
	uint8_t  (*read)(int32_t ts, uint8_t a);
	void     (*set_input)(unsigned port, const char *type, uint8_t *ptr);
	void     (*set_multitap)(unsigned sport, bool enabled);
	void     (*set_rtc)(const struct tm *ht, uint8_t lang);
	/* Watch-crystal tolerance in ppm.  Only our model has the concept;
	 * the reference backend ignores it, which is why the scenario that
	 * uses this is expected to diverge from Beetle. */
	void     (*set_rtc_oscillator)(int32_t ppm);
	void     (*transform_input)(void);
	void     (*update_input)(int32_t el);
	void     (*update_output)(void);
	void     (*reset_ts)(void);
	/* Let the surrounding system act on a change the SMPC requested
	 * (a slave-SH2 power transition).  On hardware this is the main
	 * loop; in the testbench the harness plays that role. */
	void     (*poll_system)(void);
	/* Earliest timestamp the model has asked the system to call it at,
	 * or TB_NO_EVENT.  Beetle reaches the harness through SS_SetEventNT,
	 * which its real event loop would honour; without this the two
	 * models would see different stimulus. */
	int32_t  (*earliest_event)(void);
} SmpcBackend;

extern const SmpcBackend *tb_backend;
void tb_set_backend(const SmpcBackend *be);

/* ---- stimulus helpers ---- */
#define TB_NO_EVENT 0x7FFFFFFF

int32_t tb_now(void);
/* Number of System Manager interrupts the SMPC has raised.  The BIOS's
 * handler runs on each one, which is the only thing that drives the INTBACK
 * continue/break handshake -- the SMPC will not advance to the next 32
 * nybbles until the host says so, so a harness that does not count these
 * will simply hang. */
uint32_t tb_irq_count(void);
void     tb_note_irq(void);

/* Bus helpers: these apply the SH-2 -> SMPC address decode so both backends
 * see an identical 6-bit register index. */
uint8_t tb_smpc_addr(uint8_t sh2_addr);
void     tb_write(int32_t ts, uint8_t sh2_addr, uint8_t value);
uint8_t  tb_read(int32_t ts, uint8_t sh2_addr);
void    tb_advance_to(int32_t ts);
void    tb_advance(int32_t delta);

int32_t tb_master_clock_hz(int clock_mode);
int32_t tb_smpc_master_clock(int clock_mode);
int32_t tb_line_cycles(int clock_mode);	/* one scanline, in master clocks */

/*
 * Field layout of the 16-byte input buffer each backend is handed for a
 * port, matching what the device models read (core/src/iodev.c) and what
 * Ymir and Mednafen consume.
 *
 * These are offsets *within* one port's buffer, not indices into a combined
 * multi-device buffer.  The testbench gives every port its own buffer, so a
 * scenario writes a pad's buttons at TB_PAD_3DPAD, and the device reads them
 * from offset 0 of the same buffer.  Using them as offsets into a shared
 * buffer silently dropped every scenario's stimulus: the device models saw
 * zeros, and the analog flag, the thumbsticks and the trigger values were
 * never loaded at all.
 */
enum
{
	TB_PAD_BTN    = 0,	/* 2 bytes, active high = pressed            */
	TB_PAD_THUMBX = 2,	/* 2 bytes, signed, 0x8000 = centre           */
	TB_PAD_THUMBY = 4,
	TB_PAD_SHLDL  = 6,	/* 2 bytes each, scaled to 0..255            */
	TB_PAD_SHLDR  = 8,

	/* The multi-tap's sub-slots reuse the pad layout at the same offsets,
	 * so there is no separate base for them. */
	TB_PAD_3DPAD  = TB_PAD_BTN,
	TB_PAD_GAMEPAD = TB_PAD_BTN,
	TB_PAD_MOUSE   = 0,	/* mouse: dX, dY (2 x int16), then buttons   */
	TB_PAD_WHEEL   = 0,
	TB_PAD_MISSION = 0,
	TB_PAD_GUN     = 0
};

void tb_pad_clear(uint8_t *buf);
void tb_pad_set_buttons(uint8_t *buf, uint16_t buttons);

/* ---- scenarios ---- */
typedef struct
{
	const char *name;
	void      (*run)(void);
} TbScenario;

extern const TbScenario tb_scenarios[];
extern const int         tb_scenario_count;

#endif
