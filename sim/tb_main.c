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

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s <beetle|ours> [scenario ...]\n", argv0);
	fprintf(stderr, "scenarios:\n");
	for (int i = 0; i < tb_scenario_count; i++)
		fprintf(stderr, "  %s\n", tb_scenarios[i].name);
}

int main(int argc, char **argv)
{
	const SmpcBackend *be;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "beetle"))
		be = beetle_backend();
	else if (!strcmp(argv[1], "ours"))
		be = ours_backend();
	else {
		usage(argv[0]);
		return 2;
	}
	tb_set_backend(be);

	if (argc == 2) {
		for (int i = 0; i < tb_scenario_count; i++) {
			printf("############ scenario: %s\n", tb_scenarios[i].name);
			tb_scenarios[i].run();
			trace_write(stdout);
			fflush(stdout);
		}
		return 0;
	}

	for (int a = 2; a < argc; a++) {
		int found = 0;
		for (int i = 0; i < tb_scenario_count; i++) {
			if (!strcmp(argv[a], tb_scenarios[i].name)) {
				printf("############ scenario: %s\n", tb_scenarios[i].name);
				tb_scenarios[i].run();
				trace_write(stdout);
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
