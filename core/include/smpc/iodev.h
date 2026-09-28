/*
 * iodev.h -- Saturn controller-port peripheral models.
 *
 * The SMPC is the *master* on the controller port: it drives TH (strobe) and
 * TR, and the peripheral drives TL plus a 4-bit data nibble D0-D3.  Because
 * this project replaces the SMPC itself, the core has to contain both sides
 * of that handshake:
 *
 *   - smpc.c models the master (it drives TH/TR through the pad port
 *     registers and samples the returned nibble);
 *   - this file models the peripheral, so that the two can be exercised
 *     against each other in simulation and so that a USB HID controller can
 *     be substituted for a physical one.
 *
 * Both halves share the same three-line encoding used by the real hardware
 * and by Mednafen's smpc_iodevice.c, so a peripheral model here is a
 * line-for-line port of the corresponding upstream device.
 */
#ifndef SMPC_IODEV_H
#define SMPC_IODEV_H

#include <stdint.h>
#include <stdbool.h>

typedef struct SmpcIoDev SmpcIoDev;

typedef enum
{
	SMPC_DEV_NONE = 0,
	SMPC_DEV_GAMEPAD,	/* "3-button pad", unimplemented upstream as a real device */
	SMPC_DEV_3DPAD,		/* 3D Control Pad; digital mode == Saturn Control Pad */
	SMPC_DEV_MOUSE,		/* Saturn Mouse / Shuttle Mouse */
	SMPC_DEV_WHEEL,		/* Arcade Racer */
	SMPC_DEV_MISSION,	/* Mission Stick (3- or 6-axis) */
	SMPC_DEV_GUN,		/* Virtua Gun */
	SMPC_DEV_KEYBOARD	/* Saturn Keyboard */
} SmpcDevType;

/* Port pin encoding, shared by the master and the peripheral side.
 *
 *   bit 6  TH   master -> peripheral   strobe
 *   bit 5  TR   master -> peripheral   data-valid
 *   bit 4  TL   peripheral -> master   return strobe
 *   bit 3-0 D0-3 peripheral -> master  data nibble
 *   bit 5,6,7 in the *input* direction additionally carry SC / RMD on real
 *   pads; here they are modelled as pass-through of whatever the master
 *   asserted, exactly as upstream does.
 */
#define SMPC_IO_TH 0x40
#define SMPC_IO_TR 0x20
#define SMPC_IO_TL 0x10
#define SMPC_IO_NYBBLE 0x0F

/* Allocate/destroy.  Devices are cheap; the core owns them. */
SmpcIoDev *smpc_iodev_create(SmpcDevType type);
void       smpc_iodev_free(SmpcIoDev *dev);
SmpcDevType smpc_iodev_type(const SmpcIoDev *dev);

void smpc_iodev_power(SmpcIoDev *dev);
void smpc_iodev_update_input(SmpcIoDev *dev, const uint8_t *data, int32_t time_elapsed);

/*
 * Drive the peripheral with the master's current output state and return the
 * peripheral's contribution to the port value.
 *
 *   smpc_out          the value the master is presenting (TH/TR plus whatever
 *                     else the pad port registers hold)
 *   smpc_out_asserted mask of bits the master is actively driving
 *
 * The return value is a full 8-bit port reading, with the master's asserted
 * bits passed through unchanged.
 */
uint8_t smpc_iodev_update_bus(SmpcIoDev *dev, uint8_t smpc_out, uint8_t smpc_out_asserted);

/* ---- multi-tap ---- */

typedef struct SmpcMultiTap SmpcMultiTap;

SmpcMultiTap *smpc_multitap_create(void);
void          smpc_multitap_free(SmpcMultiTap *mt);
void          smpc_multitap_set_sub(SmpcMultiTap *mt, unsigned slot, SmpcIoDev *dev);
SmpcIoDev    *smpc_multitap_get_sub(SmpcMultiTap *mt, unsigned slot);
uint8_t       smpc_multitap_update_bus(SmpcMultiTap *mt, uint8_t smpc_out, uint8_t smpc_out_asserted);

#endif
