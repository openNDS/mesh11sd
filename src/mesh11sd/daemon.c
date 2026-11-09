/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 1 stub: honour enabled/checkinterval, no mesh apply yet.
 */
#include "mesh11sd.h"

#include <stdio.h>
#include <unistd.h>

int mesh_daemon_run(struct mesh_ctx *ctx)
{
	ctx->mute = 0;
	mesh_ctx_load_setup(ctx);
	mesh_write_to_syslog(ctx, "notice",
		"Starting daemon service (v8 alpha chunk1 stub; mesh apply not yet ported)...");

	for (;;) {
		mesh_ctx_load_setup(ctx);
		if (ctx->enabled) {
			mesh_write_to_syslog(ctx, "debug",
				"daemon stub idle; waiting for later port chunks");
		}
		sleep((unsigned)ctx->checkinterval);
	}
	return 0;
}
