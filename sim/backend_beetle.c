/*
 * backend_beetle.c -- SmpcBackend implemented on top of Mednafen's SMPC.
 *
 * Deliberately thin: every scenario in tb.c must produce identical stimulus
 * against either backend, so all the logic lives in tb.c and this file only
 * adapts names.  Note that the address handed to SMPC_Read/Write here is the
 * raw SH-2 byte address; Beetle derives (A & 0x7F) >> 1 internally.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "ss.h"
#include "shim.h"
#include "smpc.h"
#include "tb.h"
#include "trace.h"

static void be_init(uint8_t area, int32_t master_clock, bool block_snd)
{
	SMPC_Init(area, master_clock, block_snd);
}

static void be_reset(bool powering_up) { SMPC_Reset(powering_up); }
static void be_start_frame(void) { SMPC_StartFrame(); }
static int32_t be_update(int32_t ts) { return SMPC_Update(ts); }
static void be_set_vbvs(int32_t ts, bool vb, bool vsync) { SMPC_SetVBVS(ts, vb, vsync); }
static void be_write(int32_t ts, uint8_t a, uint8_t v) { SMPC_Write(ts, a, v); }
static uint8_t be_read(int32_t ts, uint8_t a) { return SMPC_Read(ts, a); }
static void be_set_input(unsigned port, const char *type, uint8_t *ptr) { SMPC_SetInput(port, type, ptr); }
static void be_set_multitap(unsigned sp, bool en) { SMPC_SetMultitap(sp, en); }
static void be_set_rtc(const struct tm *ht, uint8_t lang) { SMPC_SetRTC(ht, lang); }
/* The reference counts an RTC second as 4 000 000 core clocks, with no
 * crystal tolerance to set, so there is nothing to pass on.  Our model
 * diverges from this backend by exactly the ppm given; that is the point of
 * the scenario, and it is why its column is INFO rather than PASS. */
static void be_set_rtc_oscillator(int32_t ppm) { (void)ppm; }
static void be_transform_input(void) { SMPC_TransformInput(); }
static void be_update_input(int32_t el) { SMPC_UpdateInput(el); }
static void be_update_output(void) { SMPC_UpdateOutput(); }
static void be_reset_ts(void) { SMPC_ResetTS(); }
static void be_poll_system(void) { SMPC_ProcessSlaveOffOn(); }

static int32_t be_earliest_event(void)
{
	const int32_t t = shim_earliest_event();

	shim_clear_event();
	return t;
}

static const SmpcBackend be_backend = {
	"beetle", be_init, be_reset, be_start_frame, be_update, be_set_vbvs,
	be_write, be_read, be_set_input, be_set_multitap, be_set_rtc, be_set_rtc_oscillator,
	be_transform_input, be_update_input, be_update_output, be_reset_ts, be_poll_system,
	be_earliest_event,
};

const SmpcBackend *beetle_backend(void) { return &be_backend; }
