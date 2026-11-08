/**
 * @file main.c
 * @brief Read-only planner executable for fixtures and native probes.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "mesh_config.h"
#include "platform.h"
#include "fixture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *text, uint32_t *value)
{
	unsigned long parsed;
	char *end;

	if (text == NULL || text[0] == '\0' || text[0] == '-') {
		return -1;
	}
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || *end != '\0' || parsed > UINT32_MAX) {
		return -1;
	}
	*value = (uint32_t)parsed;
	return 0;
}

int main(int argc, char **argv)
{
	struct mesh_config config;
	struct mesh_facts facts;
	struct mesh_plan plan;
	struct mesh_radio radio;
	enum mesh_error error;
	uint8_t bridge_mac[6];
	uint32_t index;
	uint32_t frequency;
	char summary[512];

	if (argc == 2 && strcmp(argv[1], "--fixture") == 0) {
		mesh_test_fixture(&config, &facts);
		error = mesh_plan_build(&config, &facts, &plan);
		if (error != MESH_OK || mesh_plan_summary(&plan, summary, sizeof(summary)) < 0) {
			fputs("test-fixture planning failed\n", stderr);
			return 1;
		}
		puts("SYNTHETIC TEST FIXTURE ONLY; no platform settings read or changed");
		puts(summary);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "--probe-bridge") == 0) {
		error = mesh_probe_bridge(argv[2], bridge_mac);
		if (error != MESH_OK) {
			fputs(mesh_error_string(error), stderr);
			fputc('\n', stderr);
			return 1;
		}
		puts("{\"bridge_identity_detected\":true,\"applied\":false}");
		return 0;
	}
	if (argc == 4 && strcmp(argv[1], "--probe-radio") == 0 &&
		number(argv[2], &index) == 0 && number(argv[3], &frequency) == 0) {
		error = mesh_probe_radio(index, frequency, &radio);
		if (error != MESH_OK) {
			fputs(mesh_error_string(error), stderr);
			fputc('\n', stderr);
			return 1;
		}
		printf("{\"mesh_supported\":%s,\"frequency_supported\":%s,"
			   "\"frequency_disabled\":%s,\"no_ir\":%s,\"radar_required\":%s,"
			   "\"combination_verified\":false,\"applied\":false}\n",
			   radio.mesh_supported ? "true" : "false",
			   radio.frequency_supported ? "true" : "false",
			   radio.frequency_disabled ? "true" : "false",
			   radio.no_initiate_radiation ? "true" : "false",
			   radio.radar_required ? "true" : "false");
		return 0;
	}
	fputs("mesh11sd v8 development core (not an installed daemon)\n"
		  "usage: mesh11sd-plan --fixture | --probe-bridge IFNAME | "
		  "--probe-radio WIPHY_INDEX FREQUENCY_MHZ\n"
		  "Read-only only; no apply/start operation exists yet.\n", stderr);
	return 2;
}
