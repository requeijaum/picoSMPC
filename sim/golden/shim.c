/*
 * shim.c -- minimal environment for running Mednafen's SMPC standalone.
 *
 * Beetle's smpc.c / smpc_iodevice.c are written against the full Mednafen
 * Saturn.  We link them unmodified against a stub of everything they touch
 * outside the SMPC, so the SMPC can serve as a golden model inside the
 * plain testbench in sim/.  Every stub that represents an externally
 * observable effect records it through the shared trace, which is the same
 * log our own core writes to -- that is what the differ compares.
 *
 * Prototype signatures here must match the real Mednafen declarations
 * exactly (ss.h / scu.h), because the golden objects are unmodified
 * upstream sources.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shim.h"
#include "mdfn_gameinfo.h"
#include "trace.h"
#include "tb.h"

/* --- event system --------------------------------------------------- */
event_list_entry events[SS_EVENT__SIMD_COUNT];

static sscpu_timestamp_t g_earliest = SS_EVENT_DISABLED_TS;

int32_t shim_earliest_event(void) { return g_earliest; }
void shim_clear_event(void) { g_earliest = SS_EVENT_DISABLED_TS; }

void SS_SetEventNT(event_list_entry *e, const sscpu_timestamp_t next_timestamp)
{
	if (e->event_time == SS_EVENT_DISABLED_TS ||
	    next_timestamp < e->event_time) {
		e->event_time = next_timestamp;
	}
	if (next_timestamp < g_earliest)
		g_earliest = next_timestamp;
}

/* The testbench honours SMPC_Update's return value rather than the event
 * list, so the event list only needs to exist for linkage. */
void SS_RequestEHLExit(void) { trace_pulse(TRACE_EFFECT_EHL_EXIT); }

/* --- SCU ------------------------------------------------------------- */
void SCU_SetInt(unsigned which, bool active)
{
	if (which == TRACE_SCU_SMPC && active)
		tb_note_irq();
	trace_line(TRACE_LINE_SCU_INTR, (int)which, active);
}

void SCU_Reset(bool hard) { (void)hard; trace_pulse(TRACE_EFFECT_SCU_RESET); }

/* --- VDP ------------------------------------------------------------- */
sscpu_timestamp_t VDP2_Update(sscpu_timestamp_t ts) { return ts; }
void VDP2_Reset(bool hard) { (void)hard; trace_pulse(TRACE_EFFECT_VDP2_RESET); }
void VDP1_Reset(bool hard) { (void)hard; trace_pulse(TRACE_EFFECT_VDP1_RESET); }
void VDP2_SetExtLatch(sscpu_timestamp_t ts, bool val)
{
	(void)ts;
	(void)val;	/* level, filtered by the trace; the PAD interrupt that
			 * accompanies it is the observable part */
}

/* --- sound ----------------------------------------------------------- */
void SOUND_Reset(bool hard) { (void)hard; trace_pulse(TRACE_EFFECT_SOUND_RESET); }

/* --- SH7095 ---------------------------------------------------------- */
void SH7095_M_SetNMI(bool level)
{
	trace_line(TRACE_LINE_SH2_NMI, 0, level);
}

void SH7095_S_SetActive(bool active)
{
	trace_line(TRACE_LINE_SH2_ACTIVE, 1, active);
}

/* --- save state (unused; satisfies smpc_iodevice.c) ------------------ */
int MDFNSS_StateAction(void *st, int load, int data_only, SFORMAT *sf,
                       const char *name, bool optional)
{
	(void)st; (void)load; (void)data_only; (void)sf; (void)name; (void)optional;
	return 0;
}

/* --- remaining symbols smpc.c / smpc_iodevice.c reference ----------- */
sscpu_timestamp_t SH7095_mem_timestamp;

void SOUND_SetClockRatio(uint32_t hz) { (void)hz; }
void SOUND_Set68KActive(bool a)       { (void)a; }
void SOUND_Reset68K(bool hard)        { (void)hard; trace_pulse(TRACE_EFFECT_SOUND_RESET); }
void CDB_SetClockRatio(uint32_t hz)   { (void)hz; }
/*
 * ss.c's SS_Reset tears the console down and calls SMPC_Reset.  Reproducing
 * that matters: SMPC_StartFrame calls SS_Reset when a SYSRES is pending, and
 * without this the SMPC's ResetPending flag would never clear and the SYSRES
 * command would never retire.
 */
void SS_Reset(bool powering_up)
{
	trace_pulse(TRACE_EFFECT_VDP_RESET);
	SMPC_Reset(powering_up);
}
int  VDP2_GetGunXTranslation(bool auto_on_screen) { (void)auto_on_screen; return 0; }

int64_t filestream_read(RFILE *s, void *b, int64_t n)  { (void)s; (void)b; (void)n; return 0; }
int64_t filestream_write(RFILE *s, const void *b, int64_t n) { (void)s; (void)b; (void)n; return 0; }

/* Frontend globals smpc_iodevice.c touches. */
int ExLatchEnable  = 0;
int ExLatchPending = 0;
int ExLatchIn      = 0;

MDFNGI *MDFNGameInfo = NULL;
int setting_gun_crosshair = 0;
