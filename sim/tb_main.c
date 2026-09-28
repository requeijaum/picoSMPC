/*
 * tb_main.c -- runs the scenarios against one backend and writes a
 * canonical trace.  Build twice (once per backend) and diff the traces.
 *
 *   ./tb_golden > golden.trace
 *   ./tb_ours   > ours.trace
 *   diff golden.trace ours.trace
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "tb.h"
#include "trace.h"

const SmpcBackend *beetle_backend(void);
const SmpcBackend *ours_backend(void);

static int g_canonical;

static void usage(const char *argv0)
{
	fprintf(stderr,
	        "usage: %s [--trace] <beetle|ours> [scenario ...]\n"
	        "  --trace   emit the canonical log (MARK/REG/EVT, no timestamps)\n"
	        "            that sim/differ.sh parses; default is the human trace\n",
	        argv0);
	fprintf(stderr, "scenarios:\n");
	for (int i = 0; i < tb_scenario_count; i++)
		fprintf(stderr, "  %s\n", tb_scenarios[i].name);
}

static void emit_trace(void)
{
	if (g_canonical)
		trace_write_canonical(stdout);
	else
		trace_write(stdout);
}

int main(int argc, char **argv)
{
	const SmpcBackend *be;
	int a = 1;

	if (a < argc && !strcmp(argv[a], "--trace")) {
		g_canonical = true;
		a++;
	}

	if (a >= argc) {
		usage(argv[0]);
		return 2;
	}
	if (!strcmp(argv[a], "beetle"))
		be = beetle_backend();
	else if (!strcmp(argv[a], "ours"))
		be = ours_backend();
	else {
		usage(argv[0]);
		return 2;
	}
	tb_set_backend(be);
	a++;

	if (a == argc) {
		for (int i = 0; i < tb_scenario_count; i++) {
			printf("############ scenario: %s\n", tb_scenarios[i].name);
			tb_scenarios[i].run();
			emit_trace();
			fflush(stdout);
		}
		return 0;
	}

	for (; a < argc; a++) {
		int found = 0;
		for (int i = 0; i < tb_scenario_count; i++) {
			if (!strcmp(argv[a], tb_scenarios[i].name)) {
				if (!g_canonical)
					printf("############ scenario: %s\n", tb_scenarios[i].name);
				else
					printf("SCEN %s\n", tb_scenarios[i].name);
				tb_scenarios[i].run();
				emit_trace();
				found = 1;
				break;
			}
		}
		if (!found) {
			fprintf(stderr, "unknown scenario: %s\n", argv[a]);
			return 2;
		}
		fflush(stdout);
	}
	return 0;
}
