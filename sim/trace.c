#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "trace.h"

static char g_lines[TRACE_MAX][160];
static int  g_count;
static int  g_ts;

static bool g_line_state[64];

void trace_reset(void)
{
	g_count = 0;
	g_ts = 0;
	memset(g_line_state, 0, sizeof g_line_state);
}

static void emit(const char *fmt, ...)
{
	va_list ap;

	if (g_count >= TRACE_MAX)
		return;
	va_start(ap, fmt);
	vsnprintf(g_lines[g_count], sizeof g_lines[0], fmt, ap);
	va_end(ap);
	g_count++;
}

static const char *scu_int_name(int id)
{
	static const char *n[] = { "VBIN", "VBOUT", "HBIN", "TIMER0",
	                           "TIMER1", "DSP", "SCSP", "SMPC", "PAD" };

	return (id >= 0 && id < 9) ? n[id] : "?";
}

void trace_pulse(TraceEffect eff)
{
	const char *s;

	switch (eff) {
	case TRACE_EFFECT_EHL_EXIT:     s = "EHL_EXIT"; break;
	case TRACE_EFFECT_SCU_RESET:    s = "SCU_RESET"; break;
	case TRACE_EFFECT_VDP1_RESET:   s = "VDP1_RESET"; break;
	case TRACE_EFFECT_VDP2_RESET:   s = "VDP2_RESET"; break;
	case TRACE_EFFECT_VDP_RESET:    s = "VDP_RESET"; break;
	case TRACE_EFFECT_SOUND_RESET:  s = "SOUND_RESET68K"; break;
	default:                        s = "PULSE"; break;
	}
	emit("[%11d]   %s", g_ts, s);
}

/*
 * Record a change of a held line.
 *
 * Both edges matter: an interrupt being released is as observable as it
 * being asserted, and a CPU losing power is not the same event as gaining
 * it.  Recording only assertions would hide half the behaviour, and
 * recording every call would bury the trace -- the models call these as
 * levels on every bus update.  So: log on transition, in either direction.
 */
void trace_line(TraceLine line, int id, bool asserted)
{
	int slot;

	switch (line) {
	case TRACE_LINE_SCU_INTR:   slot = 0x00 + (id & 0x1F); break;
	case TRACE_LINE_SH2_NMI:    slot = 0x20 + (id & 0x01); break;
	case TRACE_LINE_SH2_ACTIVE: slot = 0x22 + (id & 0x01); break;
	default:                    slot = 0x24; break;
	}

	if (g_line_state[slot] == asserted)
		return;
	g_line_state[slot] = asserted;

	switch (line) {
	case TRACE_LINE_SCU_INTR:
		emit("[%11d]   SCU_INTR_%s=%d", g_ts, scu_int_name(id), asserted);
		break;
	case TRACE_LINE_SH2_NMI:
		emit("[%11d]   SH2%d_NMI=%d", g_ts, id, asserted);
		break;
	case TRACE_LINE_SH2_ACTIVE:
		emit("[%11d]   SH2%d_ACTIVE=%d", g_ts, id, asserted);
		break;
	default:
		emit("[%11d]   SOUND_CPU_ON=%d", g_ts, asserted);
		break;
	}
}

void trace_mark(const char *label)
{
	emit("[%11d] == %s", g_ts, label);
}

void trace_reg_byte(const char *name, uint8_t value)
{
	emit("[%11d]   %-8s = %02X", g_ts, name, value);
}

void trace_reg_array(const char *name, const uint8_t *data, int len)
{
	char buf[128];
	int n = snprintf(buf, sizeof buf, "[%11d]   %-8s =", g_ts, name);

	for (int i = 0; i < len && n < (int)sizeof buf - 4; i++)
		n += snprintf(buf + n, sizeof buf - n, " %02X", data[i]);
	emit("%s", buf);
}

void trace_clock(int32_t ts)
{
	g_ts = ts;
}

int trace_write(FILE *f)
{
	for (int i = 0; i < g_count; i++)
		fprintf(f, "%s\n", g_lines[i]);
	return g_count;
}

/*
 * Strip the `[timestamp]` column and split the payload into MARK / REG / EVT.
 *
 * The stored line is always `[%11d] ` followed by either `== <label>`,
 * `   <name>       = <hex...>` or an event; the reformat is pure string
 * surgery on a format this file itself produced, so there is nothing to
 * keep in step with a second parser.
 */
int trace_write_canonical(FILE *f)
{
	int n = 0;

	for (int i = 0; i < g_count; i++) {
		const char *p = g_lines[i];
		const char *eq;
		char name[40];
		int len;

		if (strlen(p) <= 13)
			continue;
		p += 13;			/* past "[%11d]" */
		while (*p == ' ')
			p++;

		if (!strncmp(p, "== ", 3)) {
			fprintf(f, "MARK %s\n", p + 3);
			n++;
			continue;
		}

		eq = strchr(p, '=');
		if (!eq) {
			fprintf(f, "EVT  %s\n", p);
			n++;
			continue;
		}

		len = (int)(eq - p);
		while (len > 0 && p[len - 1] == ' ')
			len--;
		if (len <= 0) {
			fprintf(f, "EVT  %s\n", p);
			n++;
			continue;
		}
		if (len > (int)sizeof name - 1)
			len = (int)sizeof name - 1;
		memcpy(name, p, len);
		name[len] = 0;
		fprintf(f, "REG  %s %s\n", name, eq + 1);
		n++;
	}
	return n;
}
