/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void mesh_ctx_init(struct mesh_ctx *ctx, int is_daemon)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->debuglevel = 3;
	ctx->enabled = 1;
	ctx->commit_all = 0;
	ctx->checkinterval = 10;
	ctx->max_log_entries = 500;
	ctx->mute = is_daemon ? 0 : 1;
	ctx->pid = getpid();
	snprintf(ctx->log_mountpoint, sizeof(ctx->log_mountpoint), "/tmp");
	mkdir(MESH11SD_TMPDIR, 0755);
}

static int parse_int_default(const char *s, int def)
{
	char *end;
	long v;

	if (!s || !s[0])
		return def;
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s)
		return def;
	return (int)v;
}

int mesh_ctx_load_setup(struct mesh_ctx *ctx)
{
	char buf[256];

	if (mesh_uci_get("mesh11sd.setup.debuglevel", buf, sizeof(buf)) == 0)
		ctx->debuglevel = parse_int_default(buf, 3);
	else
		ctx->debuglevel = 3;
	if (ctx->debuglevel < 0)
		ctx->debuglevel = 0;
	if (ctx->debuglevel > 3)
		ctx->debuglevel = 3;

	if (mesh_uci_get("mesh11sd.setup.enabled", buf, sizeof(buf)) == 0)
		ctx->enabled = parse_int_default(buf, 1);
	else
		ctx->enabled = 1;

	if (mesh_uci_get("mesh11sd.setup.commit_all", buf, sizeof(buf)) == 0)
		ctx->commit_all = parse_int_default(buf, 0);
	else
		ctx->commit_all = 0;

	if (mesh_uci_get("mesh11sd.setup.checkinterval", buf, sizeof(buf)) == 0)
		ctx->checkinterval = parse_int_default(buf, 10);
	else
		ctx->checkinterval = 10;
	if (ctx->checkinterval < 1)
		ctx->checkinterval = 10;

	if (mesh_uci_get("mesh11sd.setup.max_log_entries", buf, sizeof(buf)) == 0)
		ctx->max_log_entries = parse_int_default(buf, 500);
	else
		ctx->max_log_entries = 500;

	if (mesh_uci_get("mesh11sd.setup.log_mountpoint", buf, sizeof(buf)) == 0 && buf[0]) {
		strncpy(ctx->log_mountpoint, buf, sizeof(ctx->log_mountpoint) - 1);
		ctx->log_mountpoint[sizeof(ctx->log_mountpoint) - 1] = '\0';
	} else {
		strncpy(ctx->log_mountpoint, "/tmp", sizeof(ctx->log_mountpoint) - 1);
		ctx->log_mountpoint[sizeof(ctx->log_mountpoint) - 1] = '\0';
	}

	return 0;
}
