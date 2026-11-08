/**
 * @file native_main.c
 * @brief Read-only config-check and plan command for a committed root.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "native.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

int main(int argc, char **argv)
{
	struct mesh_native_config config;
	struct mesh_facts facts;
	struct mesh_plan plan;
	enum mesh_error error;
	char summary[512];
	int root;
	bool config_only;
	struct rlimit no_core = { 0, 0 };

	if (setrlimit(RLIMIT_CORE, &no_core) != 0) {
		fputs("cannot disable confidential core dumps\n", stderr);
		return 1;
	}
	config_only = argc == 3 && strcmp(argv[1], "--config-check") == 0;
	if (!config_only && !(argc == 2 && strcmp(argv[1], "--plan") == 0)) {
		fputs("mesh11sd native development adapter: read-only\n"
			  "usage: mesh11sd-native --plan | --config-check ROOT\n"
			  "No apply/start operation is enabled.\n", stderr);
		return 2;
	}
	root = open(config_only ? argv[2] : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root < 0) {
		fputs("cannot read input root\n", stderr);
		return 1;
	}
	memset(&config, 0, sizeof(config));
	error = config_only ? mesh_native_config_read(root, &config) :
						 mesh_native_plan(root, NULL, &config, &facts, &plan);
	close(root);
	if (error != MESH_OK) {
		fputs(mesh_error_string(error), stderr);
		fputc('\n', stderr);
		mesh_native_config_clear(&config);
		return 1;
	}
	if (config_only) {
		puts("{\"committed_config_valid\":true,\"facts_verified\":false,\"applied\":false}");
	} else if (mesh_plan_summary(&plan, summary, sizeof(summary)) >= 0) {
		puts(summary);
	} else {
		mesh_native_config_clear(&config);
		return 1;
	}
	mesh_native_config_clear(&config);
	return 0;
}
