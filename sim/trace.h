/*
 * trace.h -- canonical side-effect log shared by both testbench backends.
 *
 * The SMPC's observable behaviour is not just its registers: it drives SCU
 * interrupt lines, pulses the master SH-2's NMI, gates the slave SH-2 and
 * the sound CPU, and latches the light-gun crosshair.  A register-only diff
 * would miss most real regressions, so both the golden model (Beetle, via
 * sim/golden/shim.c) and our own core record the same vocabulary here, and
 * the differ compares these logs alongside the register dumps.
 *
 * Two kinds of event:
 *
 *   trace_pulse()  an instantaneous event (a reset request, an EHL exit).
 *   trace_line()   a wire that is held at a level.
 *
 * Lines are filtered to transitions here, in one place, because both models
 * -- and the real hardware -- drive these as levels on every bus update.  A
 * single INTBACK asserts PAD hundreds of times; without the filter the trace
 * says nothing at all.
 */
#ifndef SMPC_TRACE_H
#define SMPC_TRACE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum
{
	TRACE_EFFECT_EHL_EXIT = 0,
	TRACE_EFFECT_SCU_RESET,
	TRACE_EFFECT_VDP1_RESET,
	TRACE_EFFECT_VDP2_RESET,
	TRACE_EFFECT_VDP_RESET,		/* full system reset */
	TRACE_EFFECT_SOUND_RESET	/* the 68K is held in reset */
} TraceEffect;

typedef enum
{
	TRACE_LINE_SCU_INTR,		/* id = SCU interrupt number */
	TRACE_LINE_SH2_NMI,		/* id = 0 master, 1 slave */
	TRACE_LINE_SH2_ACTIVE,		/* id = 0 master, 1 slave */
	TRACE_LINE_SOUND_ON
} TraceLine;

/* SCU interrupt numbers, matching the SMPC-relevant subset of Mednafen's
 * scu.h so the two logs are directly comparable. */
enum
{
	TRACE_SCU_VBIN = 0,
	TRACE_SCU_VBOUT,
	TRACE_SCU_HBIN,
	TRACE_SCU_TIMER0,
	TRACE_SCU_TIMER1,
	TRACE_SCU_DSP,
	TRACE_SCU_SCSP,
	TRACE_SCU_SMPC,		/* "System Manager", vector 0x47, level 8 */
	TRACE_SCU_PAD		/* controller-port latch,    vector 0x48 */
};

#define TRACE_MAX 16384

void trace_reset(void);
void trace_pulse(TraceEffect eff);
void trace_line(TraceLine line, int id, bool asserted);

void trace_mark(const char *label);
void trace_reg_byte(const char *name, uint8_t value);
void trace_reg_array(const char *name, const uint8_t *data, int len);
void trace_clock(int32_t ts);

int  trace_write(FILE *f);

/*
 * The same log in a form a script can parse: no timestamps, and registers
 * broken out as `REG <name> <bytes...>` rather than being indistinguishable
 * from an event line.  differ.sh reads this; trace_write() is for a human.
 */
int  trace_write_canonical(FILE *f);

#endif
