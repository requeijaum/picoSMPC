#ifndef SMPC_GOLDEN_SHIM_H
#define SMPC_GOLDEN_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

#include "ss.h"
#include "scu.h"
#include "smpc.h"

/*
 * The earliest timestamp the SMPC has asked the *system* to wake it at, via
 * SS_SetEventNT.  Mednafen's real ss.c consumes this in its event loop; our
 * harness has to do the same, or a host write that schedules a wakeup in one
 * master clock is only noticed at the SMPC's next internal poll -- which
 * would make the two models see different stimulus.
 */
int32_t shim_earliest_event(void);
void    shim_clear_event(void);

#endif
