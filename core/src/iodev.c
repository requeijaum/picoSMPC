/*
 * iodev.c -- Saturn controller-port peripheral models.
 *
 * Ported from Mednafen's mednafen/ss/smpc_iodevice.c (the only reference in
 * the tree that models this protocol at the pin level), restructured for a
 * flat vtable-free C core and with the multi-tap's blocking waits made
 * synchronous.  The upstream multi-tap suspends itself inside UpdateBus
 * because upstream devices can advance on their own timestamps; every device
 * modelled here answers immediately, so those waits always resolve in the
 * same call and the resumption machinery is dead weight.
 */
#include <stdlib.h>
#include <string.h>

#include "smpc/iodev.h"

struct SmpcIoDev
{
	SmpcDevType type;
	bool        powered;

	/* --- 3D Control Pad (also the Saturn Control Pad in digital mode) --- */
	uint16_t dbuttons;		/* active low, low 12 bits */
	uint8_t  thumb[2];
	uint8_t  shoulder[2];
	uint8_t  pad_buffer[16];
	uint8_t  data_out;
	bool     tl;
	int8_t   phase;

	/* --- Saturn / Shuttle Mouse --- */
	uint8_t  mouse_buttons;
	int16_t  accum_xdelta;
	int16_t  accum_ydelta;
	uint8_t  mouse_buffer[16];

	/* --- Arcade Racer --- */
	uint8_t  wheel;

	/* --- Virtua Gun --- */
	int16_t  gun_x, gun_y;
	bool     gun_trigger, gun_start, gun_off_screen, gun_never_off;
};

/* --------------------------------------------------------------------- */

static uint8_t merge_bus(uint8_t smpc_out, uint8_t smpc_out_asserted,
                         bool tl, uint8_t data_out)
{
	const uint8_t tmp = (uint8_t)((tl ? SMPC_IO_TL : 0) | (data_out & SMPC_IO_NYBBLE));

	return (uint8_t)((smpc_out & (smpc_out_asserted | 0xE0)) | (tmp & ~smpc_out_asserted));
}

SmpcIoDev *smpc_iodev_create(SmpcDevType type)
{
	SmpcIoDev *d = (SmpcIoDev *)calloc(1, sizeof(*d));

	if (!d)
		return NULL;
	d->type = type;
	smpc_iodev_power(d);
	return d;
}

void smpc_iodev_free(SmpcIoDev *d) { free(d); }
SmpcDevType smpc_iodev_type(const SmpcIoDev *d) { return d->type; }

void smpc_iodev_power(SmpcIoDev *d)
{
	d->powered = true;

	switch (d->type) {
	case SMPC_DEV_3DPAD:
		d->phase = -1;
		d->tl = true;
		d->data_out = 0x01;
		break;
	case SMPC_DEV_MOUSE:
		d->phase = -1;
		d->tl = true;
		d->data_out = 0x00;
		d->accum_xdelta = 0;
		d->accum_ydelta = 0;
		break;
	case SMPC_DEV_WHEEL:
	case SMPC_DEV_MISSION:
		d->phase = -1;
		d->tl = true;
		d->data_out = 0x01;
		break;
	case SMPC_DEV_GUN:
		d->phase = -1;
		d->tl = true;
		d->data_out = 0x0C;
		break;
	default:
		d->data_out = 0x0F;
		break;
	}
}

/* ---------------------------------------------------------------------
 * Input transform
 *
 * Layouts match what the frontend supplies, which is also what Ymir and
 * Mednafen consume:
 *   offset 0x00  2 bytes  digital/analog buttons (active low)
 *   offset 0x02  4 bytes  thumb X, 16-bit signed, 0x8000 == centre
 *   offset 0x04  4 bytes  thumb Y
 *   offset 0x06  4 bytes  shoulder L
 *   offset 0x08  4 bytes  shoulder R
 *   offset 0x00  4 bytes  mouse dX, dY (2 x int16), then 1 byte buttons
 * --------------------------------------------------------------------- */
static uint8_t scale_axis(int32_t v)
{
	/* Snap the dead centre, then map 0..65535 onto 0..255. */
	if (v >= 32768 - 128 && v < 32768)
		v = 32768;
	return (uint8_t)((v * 255 + 32767) / 65535);
}

void smpc_iodev_update_input(SmpcIoDev *d, const uint8_t *data, int32_t time_elapsed)
{
	(void)time_elapsed;

	switch (d->type) {
	case SMPC_DEV_3DPAD: {
		const uint16_t dtmp = (uint16_t)(data[0] | (data[1] << 8));
		d->dbuttons = (uint16_t)((d->dbuttons & 0x8800) | (dtmp & 0x0FFF));
		if (dtmp & 0x1000)
			d->dbuttons |= 0x8000;	/* analog mode flag */
		for (int axis = 0; axis < 2; axis++) {
			const int off = 0x2 + (axis << 1);
			const int32_t raw = (int16_t)(uint16_t)(data[off] | (data[off + 1] << 8));
			d->thumb[axis] = scale_axis(raw);
		}
		for (int w = 0; w < 2; w++) {
			const int off = 0x6 + (w << 1);
			d->shoulder[w] = scale_axis((int16_t)(uint16_t)(data[off] | (data[off + 1] << 8)));
			/* Measured hysteresis: engage by 0x8E, release by 0x55.
			 * Without the gap the digital L/R bits chatter. */
			if (d->shoulder[w] <= 0x55)
				d->dbuttons &= (uint16_t)~(0x0800 << (w << 2));
			else if (d->shoulder[w] >= 0x8E)
				d->dbuttons |= (uint16_t)(0x0800 << (w << 2));
		}
		break;
	}
	case SMPC_DEV_MOUSE:
		d->accum_xdelta = (int16_t)(d->accum_xdelta + (int16_t)(uint16_t)(data[0] | (data[1] << 8)));
		d->accum_ydelta = (int16_t)(d->accum_ydelta - (int16_t)(uint16_t)(data[2] | (data[3] << 8)));
		d->mouse_buttons = (uint8_t)(data[4] & 0xF);
		break;
	case SMPC_DEV_WHEEL: {
		const int32_t raw = (int16_t)(uint16_t)(data[0] | (data[1] << 8));
		d->wheel = (uint8_t)(1 + raw * 253 / 65534);
		if (d->wheel >= 0x6F)
			d->dbuttons &= (uint16_t)~0x0004;
		else if (d->wheel <= 0x67)
			d->dbuttons |= 0x0004;
		if (d->wheel <= 0x8F)
			d->dbuttons &= (uint16_t)~0x0008;
		else if (d->wheel >= 0x97)
			d->dbuttons |= 0x0008;
		break;
	}
	default:
		break;
	}
}

/* ---------------------------------------------------------------------
 * Bus
 * --------------------------------------------------------------------- */

static void threedpad_load_buffer(SmpcIoDev *d)
{
	const bool analog = (d->dbuttons & 0x1000) != 0;

	if (analog) {
		d->pad_buffer[ 0] = 0x1;			/* peripheral type 1 */
		d->pad_buffer[ 1] = 0x6;			/* 6 data bytes       */
		d->pad_buffer[ 2] = (uint8_t)(((d->dbuttons >>  0) & 0xF) ^ 0xF);
		d->pad_buffer[ 3] = (uint8_t)(((d->dbuttons >>  4) & 0xF) ^ 0xF);
		d->pad_buffer[ 4] = (uint8_t)(((d->dbuttons >>  8) & 0xF) ^ 0xF);
		d->pad_buffer[ 5] = (uint8_t)(((d->dbuttons >> 12) & 0xF) ^ 0xF);
		d->pad_buffer[ 6] = (uint8_t)((d->thumb[0] >> 4) & 0xF);
		d->pad_buffer[ 7] = (uint8_t)((d->thumb[0] >> 0) & 0xF);
		d->pad_buffer[ 8] = (uint8_t)((d->thumb[1] >> 4) & 0xF);
		d->pad_buffer[ 9] = (uint8_t)((d->thumb[1] >> 0) & 0xF);
		d->pad_buffer[10] = (uint8_t)((d->shoulder[0] >> 4) & 0xF);
		d->pad_buffer[11] = (uint8_t)((d->shoulder[0] >> 0) & 0xF);
		d->pad_buffer[12] = (uint8_t)((d->shoulder[1] >> 4) & 0xF);
		d->pad_buffer[13] = (uint8_t)((d->shoulder[1] >> 0) & 0xF);
		d->pad_buffer[14] = 0x0;
		d->pad_buffer[15] = 0x1;
	} else {
		/* Digital mode: the report is the Saturn Control Pad's two
		 * button bytes.  The phase jumps straight to the data so the
		 * packet is short. */
		d->phase = 8;
		d->pad_buffer[ 8] = 0x0;			/* type 0, 2 bytes   */
		d->pad_buffer[ 9] = 0x2;
		d->pad_buffer[10] = (uint8_t)(((d->dbuttons >>  0) & 0xF) ^ 0xF);
		d->pad_buffer[11] = (uint8_t)(((d->dbuttons >>  4) & 0xF) ^ 0xF);
		d->pad_buffer[12] = (uint8_t)(((d->dbuttons >>  8) & 0xF) ^ 0xF);
		d->pad_buffer[13] = (uint8_t)(((d->dbuttons >> 12) & 0xF) ^ 0xF);
		d->pad_buffer[14] = 0x0;
		d->pad_buffer[15] = 0x1;
	}
}

static void tl_protocol_bus(SmpcIoDev *d, uint8_t smpc_out)
{
	if (smpc_out & SMPC_IO_TH) {
		d->phase = -1;
		d->tl = true;
		d->data_out = 0x01;
		return;
	}
	if ((smpc_out & SMPC_IO_TR) != (d->tl ? SMPC_IO_TR : 0)) {
		if (d->phase < 15) {
			d->tl = !d->tl;
			d->phase++;
		}
		if (!d->phase)
			threedpad_load_buffer(d);
		d->data_out = d->pad_buffer[d->phase];
	}
}

static uint8_t digital_bus(SmpcIoDev *d, uint8_t smpc_out, uint8_t smpc_out_asserted)
{
	/* A plain 3-button pad has no packet protocol: it returns a button
	 * nibble selected by the TH/TR level, with TL held high. */
	const uint8_t tmp = (uint8_t)((d->dbuttons >> (((smpc_out >> 5) & 3) << 2)) & 0xF);

	return (uint8_t)(0x10 | (smpc_out & (smpc_out_asserted | 0xE0)) | (tmp & ~smpc_out_asserted));
}

uint8_t smpc_iodev_update_bus(SmpcIoDev *d, uint8_t smpc_out, uint8_t smpc_out_asserted)
{
	if (!d)
		return 0x7F;

	switch (d->type) {
	case SMPC_DEV_NONE:
		/* Idle: TH, TR and TL high, data nibble high. */
		return (uint8_t)((smpc_out & (smpc_out_asserted | 0xE0)) | 0x7F);

	case SMPC_DEV_GAMEPAD:
		return digital_bus(d, smpc_out, smpc_out_asserted);

	case SMPC_DEV_3DPAD:
	case SMPC_DEV_WHEEL:
	case SMPC_DEV_MISSION:
	case SMPC_DEV_KEYBOARD:
		tl_protocol_bus(d, smpc_out);
		return merge_bus(smpc_out, smpc_out_asserted, d->tl, d->data_out);

	case SMPC_DEV_MOUSE: {
		if (smpc_out & SMPC_IO_TH) {
			/* The mouse is the odd one out: TH+TR together is its
			 * "begin transmission, latch the accumulated delta"
			 * command, not a plain reset.  Polling it wrong loses
			 * motion, so this asymmetry is load-bearing. */
			if (smpc_out & SMPC_IO_TR) {
				if (!d->tl)
					d->accum_xdelta = d->accum_ydelta = 0;
				d->phase = -1;
				d->tl = true;
				d->data_out = 0x00;
			} else {
				if (d->tl)
					d->tl = false;
			}
		} else {
			if (d->phase < 0) {
				uint8_t flags = 0;
				if (d->accum_xdelta < 0)  flags |= 0x1;
				if (d->accum_ydelta < 0)  flags |= 0x2;
				if (d->accum_xdelta > 255 || d->accum_xdelta < -256) {
					flags |= 0x4;
					d->accum_xdelta = (int16_t)(d->accum_xdelta < 0 ? -256 : 255);
				}
				if (d->accum_ydelta > 255 || d->accum_ydelta < -256) {
					flags |= 0x8;
					d->accum_ydelta = (int16_t)(d->accum_ydelta < 0 ? -256 : 255);
				}
				d->mouse_buffer[0] = 0xB;	/* peripheral id 0xB */
				d->mouse_buffer[1] = 0xF;	/* 3 data bytes      */
				d->mouse_buffer[2] = 0xF;
				d->mouse_buffer[3] = flags;
				d->mouse_buffer[4] = d->mouse_buttons;
				d->mouse_buffer[5] = (uint8_t)((d->accum_xdelta >> 4) & 0xF);
				d->mouse_buffer[6] = (uint8_t)((d->accum_xdelta >> 0) & 0xF);
				d->mouse_buffer[7] = (uint8_t)((d->accum_ydelta >> 4) & 0xF);
				d->mouse_buffer[8] = (uint8_t)((d->accum_ydelta >> 0) & 0xF);
				for (int i = 9; i < 16; i++)
					d->mouse_buffer[i] = d->mouse_buffer[8];
				d->phase++;
			}
			if ((bool)(smpc_out & SMPC_IO_TR) != d->tl) {
				d->phase = (int8_t)((d->phase + 1) & 0xF);
				d->tl = !d->tl;
				if (d->phase == 8)
					d->accum_xdelta = d->accum_ydelta = 0;
			}
			d->data_out = d->mouse_buffer[d->phase];
		}
		return merge_bus(smpc_out, smpc_out_asserted, d->tl, d->data_out);
	}

	case SMPC_DEV_GUN:
		/* The gun is not polled through TH/TR at all: the SMPC reads it
		 * with the pad port configured as a plain input and EXLE set, and
		 * a TH reading low is what latches the crosshair position. */
		return (uint8_t)((smpc_out & (smpc_out_asserted | 0xE0)) | 0x1C);

	default:
		return merge_bus(smpc_out, smpc_out_asserted, true, 0x0F);
	}
}

/* ---------------------------------------------------------------------
 * Multi-tap
 *
 * The six-player adapter sits *between* the SMPC and up to six pads, and
 * re-uses the same TL-negotiated nibble protocol.  Its reply to a TH strobe
 * is a fixed program of nibbles:
 *
 *     0x4 0x1 0x6 0x0            adapter id / sub-slot count
 *     for each of the 6 slots:
 *         probe the sub-pad to learn its generation (id1)
 *         0xB (digital)     0x0 0x2, then the two button bytes, low nybble first
 *         0x3/0x5 (analog)  id2 high/low, then `id2 & 0xF` data bytes
 *         anything else     0xF 0xF
 *     0x0 0x1                    terminator
 *
 * Upstream implements this as a self-suspending coroutine inside UpdateBus,
 * because its devices can advance on their own timestamps.  Every device
 * modelled here answers synchronously, so the program is instead run to
 * completion once per TH strobe and the resulting nibbles are replayed one
 * per TR edge.  The sub-pads see exactly the same sub_state sequence either
 * way, so their end state -- and therefore the nibble stream -- is identical.
 */
#define MT_NIBBLES 256

struct SmpcMultiTap
{
	SmpcIoDev *subs[6];

	uint8_t stream[MT_NIBBLES];
	int     stream_len;
	int     cursor;

	bool    tl;
	uint8_t data_out;
};

SmpcMultiTap *smpc_multitap_create(void)
{
	return (SmpcMultiTap *)calloc(1, sizeof(SmpcMultiTap));
}

void smpc_multitap_free(SmpcMultiTap *mt) { free(mt); }

void smpc_multitap_set_sub(SmpcMultiTap *mt, unsigned slot, SmpcIoDev *dev)
{
	if (slot < 6)
		mt->subs[slot] = dev;
}

SmpcIoDev *smpc_multitap_get_sub(SmpcMultiTap *mt, unsigned slot)
{
	return (slot < 6) ? mt->subs[slot] : NULL;
}

/* The sub-pad's three-line sub-bus.  sub_state drives TH/TR; asserted is
 * 0x60 (TH and TR both driven by the adapter) or 0x40. */
static uint8_t sub_bus(SmpcMultiTap *mt, unsigned slot, uint8_t sub_state)
{
	if (!mt->subs[slot])
		return (uint8_t)(0x7F);	/* nothing plugged into that slot */
	return smpc_iodev_update_bus(mt->subs[slot], sub_state, 0x60);
}

static void mt_emit(SmpcMultiTap *mt, uint8_t v)
{
	if (mt->stream_len < MT_NIBBLES)
		mt->stream[mt->stream_len++] = (uint8_t)(v & SMPC_IO_NYBBLE);
}

/* De-scramble a nybble pair into an id, the same way the SMPC's master
 * side does.  Real pads wire the four data lines out of order relative to
 * the master's sampling, so the master recombines adjacent bit pairs. */
static uint8_t nibble_pair(uint8_t hi, uint8_t lo)
{
	return (uint8_t)(((((hi >> 3) | (hi >> 2)) & 1) << 3) |
	                 ((((hi >> 1) | (hi >>  0)) & 1) << 2) |
	                 ((((lo >> 3) | (lo >> 2)) & 1) << 1) |
	                 ((((lo >> 1) | (lo >>  0)) & 1) << 0));
}

static void mt_build_stream(SmpcMultiTap *mt)
{
	static const uint8_t header[4] = { 0x4, 0x1, 0x6, 0x0 };

	mt->stream_len = 0;
	mt->cursor = 0;

	for (int i = 0; i < 4; i++)
		mt_emit(mt, header[i]);

	for (unsigned slot = 0; slot < 6; slot++) {
		const uint8_t hi = sub_bus(mt, slot, 0x60);
		const uint8_t lo = sub_bus(mt, slot, 0x20);
		const uint8_t id1 = nibble_pair(hi, lo);

		if (id1 == 0xB) {
			/* Saturn Control Pad generation: it answers the id probe
			 * directly instead of negotiating, so the adapter can
			 * synthesise the whole report from four samples. */
			const uint8_t a = sub_bus(mt, slot, 0x40);
			const uint8_t b = sub_bus(mt, slot, 0x00);

			mt_emit(mt, 0x0);
			mt_emit(mt, 0x2);
			mt_emit(mt, lo & 0xF);
			mt_emit(mt, a  & 0xF);
			mt_emit(mt, b  & 0xF);
			mt_emit(mt, (hi & 0xF) | 0x7);
		} else if (id1 == 0x3 || id1 == 0x5) {
			/* TL-negotiated generation: ask the pad for its own id and
			 * data size, then stream that many bytes. */
			const uint8_t dhi = sub_bus(mt, slot, 0x00);
			const uint8_t dlo = sub_bus(mt, slot, 0x20);
			uint8_t id2 = (uint8_t)((((dhi & 0xF) << 4) | (dlo & 0xF)) & 0xFF);

			if (id1 == 0x3)
				id2 = 0xE3;	/* Saturn Mouse / Shuttle Mouse */

			mt_emit(mt, (uint8_t)(id2 >> 4));
			mt_emit(mt, (uint8_t)(id2 & 0xF));

			for (int n = 0; n < (id2 & 0xF); n++) {
				const uint8_t bhi = sub_bus(mt, slot, 0x00);
				const uint8_t blo = sub_bus(mt, slot, 0x20);
				mt_emit(mt, bhi & 0xF);
				mt_emit(mt, blo & 0xF);
			}
		} else {
			mt_emit(mt, 0xF);
			mt_emit(mt, 0xF);
		}

		/* Leave the sub-pad parked in its idle state. */
		(void)sub_bus(mt, slot, 0x60);
	}

	mt_emit(mt, 0x0);
	mt_emit(mt, 0x1);
}

uint8_t smpc_multitap_update_bus(SmpcMultiTap *mt, uint8_t smpc_out, uint8_t smpc_out_asserted)
{
	if (smpc_out & SMPC_IO_TH) {
		mt_build_stream(mt);
		mt->tl = true;
		mt->data_out = 0x01;
		return merge_bus(smpc_out, smpc_out_asserted, mt->tl, mt->data_out);
	}

	/* Each TR edge, opposite to TL, shifts out the next nybble. */
	if ((bool)(smpc_out & SMPC_IO_TR) != mt->tl) {
		mt->tl = !mt->tl;
		mt->data_out = (mt->cursor < mt->stream_len) ? mt->stream[mt->cursor] : 0x00;
		if (mt->cursor < mt->stream_len)
			mt->cursor++;
	}

	return merge_bus(smpc_out, smpc_out_asserted, mt->tl, mt->data_out);
}
