/*
 * smpc.h -- SMPC (System Manager & Peripheral Control) core.
 *
 * Behavioural reimplementation of the Sega Saturn's SMPC, the Hitachi
 * HD404920FS / Sega 315-5744 (HMCS400 core, 4 MHz).  Portable C99, no
 * dependencies, no floating point, no allocation after init -- so the same
 * object code runs in the native testbench and on the RP2350.
 *
 * Address map.  The SMPC's six address pins are the SH-2's A2..A7, so the
 * register index is (sh2_byte_address & 0x7F) >> 1 and only odd byte
 * addresses decode; the caller applies that shift and hands the core the
 * 6-bit index.  Indices 0x40..0x7F are not decoded (the SMPC only has six
 * address lines), so the 0x40 mirror is handled by masking.
 *
 * In SH-2 byte addresses:
 *
 *   0x01..0x0D  IREG0..IREG6   write-only command arguments
 *   0x1F        COMREG         write: start a command; read: OREG31
 *   0x21..0x5F  OREG0..OREG31  32 response bytes
 *   0x61        SR             status: PDL | NPE | RESB | P2MD | P1MD
 *   0x63        SF             bit 0: 1 = command in progress
 *   0x75..0x77  PDR1/PDR2      controller port, 7 bits used
 *   0x79..0x7B  DDR1/DDR2      0 = drive, 1 = release to input
 *   0x7D        IOSEL          bit 0/1: 1 = SH-2 drives the port directly
 *   0x7F        EXLE           bit 0/1: external latch enable (light gun)
 *
 * A read of an address with no register returns the last value written to
 * the port -- the SMPC's data bus is open, and software (and Beetle) relies
 * on bit 7 of a PDR read and bits 7..1 of an SF read coming from there.
 *
 * Timing is in SMPC clocks: the HMCS400 core runs at 4 MHz, so one clock is
 * 250 ns.  smpc_run() is driven in SH-2 master-clock timestamps and converts
 * internally, exactly as Mednafen does, so that the clock-domain ratio is
 * part of the model rather than an approximation of it.
 */
#ifndef SMPC_H
#define SMPC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "smpc/iodev.h"

/* ---- SH-2 register addresses (see header comment) ---- */
enum
{
	SMPC_IREG0 = 0x01,
	SMPC_IREG1 = 0x03,
	SMPC_IREG2 = 0x05,
	SMPC_IREG3 = 0x07,
	SMPC_IREG4 = 0x09,
	SMPC_IREG5 = 0x0B,
	SMPC_IREG6 = 0x0D,
	SMPC_COMREG = 0x1F,
	SMPC_OREG0 = 0x21,
	SMPC_OREG31 = 0x5F,
	SMPC_SR = 0x61,
	SMPC_SF = 0x63,
	SMPC_PDR1 = 0x75,
	SMPC_PDR2 = 0x77,
	SMPC_DDR1 = 0x79,
	SMPC_DDR2 = 0x7B,
	SMPC_IOSEL = 0x7D,
	SMPC_EXLE = 0x7F
};

/* ---- commands (SH2CMD) ---- */
enum
{
	SMPC_CMD_MSHON = 0x00,
	SMPC_CMD_SSHON = 0x02,
	SMPC_CMD_SSHOFF = 0x03,
	SMPC_CMD_SNDON = 0x06,
	SMPC_CMD_SNDOFF = 0x07,
	SMPC_CMD_CDON = 0x08,
	SMPC_CMD_CDOFF = 0x09,
	SMPC_CMD_NETLINKON = 0x0A,	/* undocumented; present in the ROM */
	SMPC_CMD_NETLINKOFF = 0x0B,	/* undocumented; present in the ROM */
	SMPC_CMD_SYSRES = 0x0D,
	SMPC_CMD_CKCHG352 = 0x0E,
	SMPC_CMD_CKCHG320 = 0x0F,
	SMPC_CMD_INTBACK = 0x10,
	SMPC_CMD_SETTIME = 0x16,
	SMPC_CMD_SETSMEM = 0x17,
	SMPC_CMD_NMIREQ = 0x18,
	SMPC_CMD_RESENAB = 0x19,
	SMPC_CMD_RESDISA = 0x1A,
	SMPC_CMD_SEC_GETSEED = 0x1E,	/* undocumented, copy protection */
	SMPC_CMD_SEC_VERIFY = 0x1F	/* undocumented, copy protection */
};

/* ---- status register bits ---- */
enum
{
	SMPC_SR_RESB = 0x10,	/* reset button held */
	SMPC_SR_NPE  = 0x20,	/* peripheral data remains */
	SMPC_SR_PDL  = 0x40	/* first data block */
	/* bit 7 distinguishes a peripheral report (1) from a status report (0);
	 * bits 3..0 are the two port modes. */
};

/* ---- SR port-mode encodings (SH2CMD1 bits 4-5 / 6-7) ---- */
enum
{
	SMPC_PMODE_15BYTE = 0,
	SMPC_PMODE_255BYTE = 1,
	SMPC_PMODE_ILL = 2,
	SMPC_PMODE_0BYTE = 3
};

/* ---- SCU interrupt ids the SMPC can raise ---- */
enum
{
	SMPC_SCU_INT_SMPC = 7,	/* "System Manager", vector 0x47, level 8 */
	SMPC_SCU_INT_PAD = 8	/* controller-port external latch, vector 0x48 */
};

/* ---- area codes (OREG9) ---- */
enum
{
	SMPC_AREA_JP = 0x1,
	SMPC_AREA_ASIA_NTSC = 0x2,
	SMPC_AREA_NA = 0x4,
	SMPC_AREA_CSA_NTSC = 0x5,
	SMPC_AREA_KR = 0x6,
	SMPC_AREA_ASIA_PAL = 0xA,
	SMPC_AREA_EU_PAL = 0xC,
	SMPC_AREA_CSA_PAL = 0xD
};

/* Clock divisors, as in Mednafen: the SH-2 timestamp domain is
 * master_clock / divisor. */
#define SMPC_CLOCK_DIVISOR_26M 65
#define SMPC_CLOCK_DIVISOR_28M 61

/*
 * Environment callbacks.  Everything the SMPC does that reaches outside its
 * own register file goes through here, so the core has no dependency on a
 * VDP, an SCU or a CPU model -- and so the RP2350 port only has to supply
 * these.
 */
typedef struct
{
	void *ctx;

	void (*scu_int)(void *ctx, unsigned int which, bool active);
	void (*sh2_nmi)(void *ctx, unsigned int sh2, bool level);
	void (*sh2_active)(void *ctx, unsigned int sh2, bool active);
	void (*vdp_reset)(void *ctx);
	void (*scu_reset)(void *ctx);
	void (*sound_reset)(void *ctx);
	void (*ext_latch)(void *ctx, bool latched);
	void (*request_ehl_exit)(void *ctx);
} SmpcEnv;

typedef struct Smpc Smpc;

/*
 * Smpc is opaque so the register file's layout stays private, but the core
 * allocates nothing: callers place one in static storage.  This returns the
 * size and alignment it needs, which also keeps the RP2350 port free of a
 * heap.
 */
size_t smpc_sizeof(void);
size_t smpc_alignof(void);

void smpc_init(Smpc *s, const SmpcEnv *env, uint8_t area_code,
               int32_t master_clock, bool block_sound_cpu_control);
void smpc_reset(Smpc *s, bool powering_up);
void smpc_kill(Smpc *s);

void smpc_set_rtc(Smpc *s, const struct tm *ht, uint8_t lang);
void smpc_get_rtc(Smpc *s, uint8_t out[7]);
void smpc_set_smem(Smpc *s, const uint8_t smem[4]);

/*
 * The RTC's tolerance, in parts per million, of the 32.768 kHz watch crystal
 * on OSC1/OSC2.  That oscillator is a separate part from the 4 MHz one that
 * clocks the core, so the two drift apart on a real console; 0 ppm, the
 * default, is nominal and reproduces Mednafen's behaviour exactly.  The core
 * has no notion of absolute time, so this does not make the model more
 * accurate by itself -- it makes the model able to say which crystal it is
 * imitating.  Survives smpc_reset(). */
void smpc_set_rtc_oscillator(Smpc *s, int32_t ppm);
void smpc_get_smem(Smpc *s, uint8_t out[4]);

/* Peripherals.  Ports 0 and 1 are the front panel; 2..5 are the sub-slots
 * of a multi-tap. */
void smpc_set_peripheral(Smpc *s, unsigned port, SmpcDevType type);
void smpc_set_peripheral_dev(Smpc *s, unsigned port, SmpcIoDev *dev, bool take_ownership);
void smpc_set_multitap(Smpc *s, unsigned port, bool enabled);
void smpc_update_input(Smpc *s, unsigned port, const uint8_t *data);
void smpc_set_reset_button(Smpc *s, bool pressed);
void smpc_set_area_code(Smpc *s, uint8_t code);

/*
 * The bus.  Addresses are raw SH-2 byte addresses; the core does the
 * (addr & 0x7F) >> 1 shift itself.  Time is a SH-2 master-clock timestamp.
 */
uint8_t  smpc_read(Smpc *s, int32_t ts, uint8_t addr);
void     smpc_write(Smpc *s, int32_t ts, uint8_t addr, uint8_t value);

void smpc_set_vbvs(Smpc *s, int32_t ts, bool vb, bool vsync);
int32_t  smpc_run(Smpc *s, int32_t ts);
void     smpc_start_frame(Smpc *s);
/* Apply a slave-SH2 power change the SMPC has requested.  On real hardware
 * the surrounding system performs this; in the testbench the harness does. */
void     smpc_poll_system(Smpc *s);

bool     smpc_reset_pending(const Smpc *s);

/* Introspection, for the testbench and for a hardware bring-up where the
 * only way to see inside the chip is the debug probe. */
int     smpc_debug_command(const Smpc *s);
int     smpc_debug_phase(const Smpc *s);
int64_t smpc_debug_clock_counter(const Smpc *s);
uint8_t  smpc_read_reg(Smpc *s, uint8_t internal_addr);

#endif
