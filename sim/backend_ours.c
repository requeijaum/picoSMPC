/*
 * backend_ours.c -- SmpcBackend implemented on our own core.
 *
 * Signature-for-signature equivalent of backend_beetle.c, including the
 * "misc" port (12) which on our side feeds the reset button, and the system
 * poll that completes a requested slave-SH2 power change.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <assert.h>

#include "smpc/smpc.h"
#include "tb.h"
#include "trace.h"

/* The core is opaque and heap-free, so the backend owns one in static
 * storage.  The size is checked at compile time so a growing register file
 * cannot silently overflow it. */
/* 4 KiB of static storage: the core's footprint is a couple of kilobytes,
 * dominated by the report sequencer's program buffer.  The static assert
 * below keeps a growing register file from silently overflowing it. */
enum { SMPC_STORAGE_BYTES = 4096 };

static union
{
	uint64_t align;
	uint8_t bytes[SMPC_STORAGE_BYTES];
} g_smpc_storage;

#define g_smpc (*(Smpc *)g_smpc_storage.bytes)


static uint8_t g_reset_button;

/* ---- environment callbacks ---- */

static void ev_scu_int(void *ctx, unsigned which, bool active)
{
	(void)ctx;
	if (which == TRACE_SCU_SMPC && active)
		tb_note_irq();
	trace_line(TRACE_LINE_SCU_INTR, (int)which, active);
}

static void ev_sh2_nmi(void *ctx, unsigned sh2, bool level)
{
	(void)ctx;
	trace_line(TRACE_LINE_SH2_NMI, (int)sh2, level);
}

static void ev_sh2_active(void *ctx, unsigned sh2, bool active)
{
	(void)ctx;
	trace_line(TRACE_LINE_SH2_ACTIVE, (int)sh2, active);
}

static void ev_vdp_reset(void *ctx) { (void)ctx; trace_pulse(TRACE_EFFECT_VDP_RESET); }
static void ev_scu_reset(void *ctx) { (void)ctx; trace_pulse(TRACE_EFFECT_SCU_RESET); }
static void ev_sound_reset(void *ctx) { (void)ctx; trace_pulse(TRACE_EFFECT_SOUND_RESET); }
static void ev_request_ehl_exit(void *ctx) { (void)ctx; trace_pulse(TRACE_EFFECT_EHL_EXIT); }
static void ev_ext_latch(void *ctx, bool latched) { (void)ctx; (void)latched; }

/* The SmpcEnv is const, so the context pointer has to be a real object; the
 * callbacks ignore it, so a shared dummy is enough. */
static int g_env_ctx;

static const SmpcEnv g_env = {
	&g_env_ctx,
	ev_scu_int, ev_sh2_nmi, ev_sh2_active, ev_vdp_reset, ev_scu_reset,
	ev_sound_reset, ev_ext_latch, ev_request_ehl_exit
};

/* ---- input plumbing ---- */

#define MAX_PORTS 6
static uint8_t *g_input[MAX_PORTS];
static const char *g_type[MAX_PORTS];

static void remap(unsigned port)
{
	if (port >= 2)
		return;
	if (g_type[port] && !strcmp(g_type[port], "none"))
		smpc_set_multitap(&g_smpc, port, false);
}

static void be_init(uint8_t area, int32_t master_clock, bool block_snd)
{
	assert(smpc_sizeof() <= SMPC_STORAGE_BYTES);
	smpc_init(&g_smpc, &g_env, area, master_clock, block_snd);
}

static void be_reset(bool powering_up) { smpc_reset(&g_smpc, powering_up); }
static void be_start_frame(void) { smpc_start_frame(&g_smpc); }
static int32_t be_update(int32_t ts) { return smpc_run(&g_smpc, ts); }
static void be_set_vbvs(int32_t ts, bool vb, bool vsync) { smpc_set_vbvs(&g_smpc, ts, vb, vsync); }
static void be_write(int32_t ts, uint8_t a, uint8_t v) { smpc_write(&g_smpc, ts, a, v); }
static uint8_t be_read(int32_t ts, uint8_t a) { return smpc_read(&g_smpc, ts, a); }

static void be_set_input(unsigned port, const char *type, uint8_t *ptr)
{
	if (port == 12) {
		g_reset_button = ptr[0] & 1;
		smpc_set_reset_button(&g_smpc, g_reset_button != 0);
		return;
	}
	if (port >= MAX_PORTS)
		return;

	g_type[port] = type;
	g_input[port] = ptr;

	if (!strcmp(type, "none")) {
		smpc_set_peripheral(&g_smpc, port, SMPC_DEV_NONE);
	} else if (!strcmp(type, "gamepad")) {
		smpc_set_peripheral(&g_smpc, port, SMPC_DEV_GAMEPAD);
	} else if (!strcmp(type, "3dpad")) {
		smpc_set_peripheral(&g_smpc, port, SMPC_DEV_3DPAD);
	} else if (!strcmp(type, "mouse")) {
		smpc_set_peripheral(&g_smpc, port, SMPC_DEV_MOUSE);
	} else if (!strcmp(type, "wheel")) {
		smpc_set_peripheral(&g_smpc, port, SMPC_DEV_WHEEL);
	} else if (!strcmp(type, "gun")) {
		smpc_set_peripheral(&g_smpc, port, SMPC_DEV_GUN);
	}
	remap(port);
}

static void be_set_multitap(unsigned sp, bool en) { smpc_set_multitap(&g_smpc, sp, en); }
static void be_set_rtc(const struct tm *ht, uint8_t lang) { smpc_set_rtc(&g_smpc, ht, lang); }
static void be_set_rtc_oscillator(int32_t ppm) { smpc_set_rtc_oscillator(&g_smpc, ppm); }
static void be_transform_input(void) { }

/*
 * Deliberately *not* done from be_set_input.
 *
 * Beetle's SMPC_SetInput only stores the pointer; the device sees its input
 * when SMPC_UpdateInput runs.  Loading ours eagerly meant the two backends
 * sampled their stimulus at different points -- ours on set_input, Beetle's
 * on update_input -- so for the scenarios that never call update_input they
 * were being tested against different inputs.  Loading here keeps the
 * stimulus identical, which is the whole point of having one testbench.
 */
static void be_update_input(int32_t el)
{
	(void)el;
	for (unsigned port = 0; port < MAX_PORTS; port++)
		if (g_input[port])
			smpc_update_input(&g_smpc, port, g_input[port]);
}
static void be_update_output(void) { }
static void be_reset_ts(void) { }
static void be_poll_system(void) { smpc_poll_system(&g_smpc); }
static int32_t be_earliest_event(void) { return TB_NO_EVENT; }

static const SmpcBackend be_backend = {
	"ours", be_init, be_reset, be_start_frame, be_update, be_set_vbvs,
	be_write, be_read, be_set_input, be_set_multitap, be_set_rtc, be_set_rtc_oscillator,
	be_transform_input, be_update_input, be_update_output, be_reset_ts, be_poll_system,
	be_earliest_event,
};

const SmpcBackend *ours_backend(void) { return &be_backend; }
