/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <string.h>

int main(int argc, char **argv)
{
	struct mesh_ctx ctx;
	int is_daemon = (argc >= 2 && !strcmp(argv[1], "daemon"));

	mesh_ctx_init(&ctx, is_daemon);
	return mesh_cli_dispatch(&ctx, argc, argv);
}
