/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mesh11sd ubus object, run as "mesh11sd ubus" in its own procd instance.
 *
 * It answers local ubus calls with the same functions the command line
 * uses and never touches the daemon's loop. The only changes it can make
 * are to ask the daemon to reload its configuration (config_reload) and to
 * start the auto_config confidence test (auto_config_test).
 * Logging is muted, as for the command line, except for errors.
 */
#include "mesh11sd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>

#ifndef MESH11SD_BUILD_COMMIT
#define MESH11SD_BUILD_COMMIT "unknown"
#endif

static struct mesh_ctx *mctx;
static struct blob_buf bb;

static void ubus_log_err(const char *msg)
{
	mctx->mute = 0;
	mesh_log_err(mctx, "ubus: %s", msg);
	mctx->mute = 1;
}

/* Trim leading and trailing white space in place. */
static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

/* ubus call mesh11sd status: the mesh11sd status JSON */
static int ubus_status(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	static char text[65536];

	(void)obj; (void)method; (void)msg;
	if (mesh_status_capture(mctx, text, sizeof(text)) != 0)
		return UBUS_STATUS_UNKNOWN_ERROR;
	blob_buf_init(&bb, 0);
	if (!blobmsg_add_json_from_string(&bb, text)) {
		ubus_log_err("status output is not valid JSON");
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	ubus_send_reply(ctx, req, bb.head);
	return 0;
}

/*
 * One "iw dev <ifname> station dump" as an array of tables:
 *   Station <mac> (on <ifname>)      -> { "mac": "<mac>",
 *   	<key>:	<value>                    "<key>": "<value>", ... }
 */
static void add_station_dump(const char *ifname)
{
	static char out[65536];
	char *argv[] = { "iw", "dev", (char *)ifname, "station", "dump", NULL };
	char *p = out, *line;
	void *arr, *tbl = NULL;

	arr = blobmsg_open_array(&bb, ifname);
	if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
		while ((line = strsep(&p, "\n")) != NULL) {
			char mac[32], *colon;

			if (sscanf(line, "Station %31s", mac) == 1) {
				if (tbl)
					blobmsg_close_table(&bb, tbl);
				tbl = blobmsg_open_table(&bb, NULL);
				blobmsg_add_string(&bb, "mac", mac);
				continue;
			}
			colon = strchr(line, ':');
			if (!tbl || !isspace((unsigned char)line[0]) || !colon)
				continue;
			*colon = '\0';
			blobmsg_add_string(&bb, trim(line), trim(colon + 1));
		}
		if (tbl)
			blobmsg_close_table(&bb, tbl);
	}
	blobmsg_close_array(&bb, arr);
}

/* ubus call mesh11sd stations: one-hop peers on each mesh interface */
static int ubus_stations(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	void *ifs;
	int i;

	(void)obj; (void)method; (void)msg;
	mesh_get_mesh_iflist(mctx);
	blob_buf_init(&bb, 0);
	ifs = blobmsg_open_table(&bb, "interfaces");
	for (i = 0; i < mctx->iflist_count; i++)
		add_station_dump(mctx->iflist[i]);
	blobmsg_close_table(&bb, ifs);
	ubus_send_reply(ctx, req, bb.head);
	return 0;
}

/* ubus call mesh11sd config_reload: as mesh11sd config_reload */
static int ubus_config_reload(struct ubus_context *ctx, struct ubus_object *obj,
			      struct ubus_request_data *req, const char *method,
			      struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	if (mesh_request_config_reload() != 0) {
		ubus_log_err("unable to request a configuration reload");
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	blob_buf_init(&bb, 0);
	blobmsg_add_u8(&bb, "requested", 1);
	ubus_send_reply(ctx, req, bb.head);
	return 0;
}

/*
 * ubus call mesh11sd auto_config_test: start the auto_config confidence
 * test, as "mesh11sd auto_config test" does after its yes/no question.
 * Refused (with a message) while auto_config is on or a test is running.
 */
static int ubus_auto_config_test(struct ubus_context *ctx, struct ubus_object *obj,
				 struct ubus_request_data *req, const char *method,
				 struct blob_attr *msg)
{
	const char *refused = NULL;

	(void)obj; (void)method; (void)msg;
	mesh_ctx_load_setup(mctx);
	switch (mesh_auto_config_test_check(mctx)) {
	case MESH_AUTO_CONFIG_TEST_ON:
		refused = "auto_config is already enabled, no need to test";
		break;
	case MESH_AUTO_CONFIG_TEST_RUNNING:
		refused = "Auto config test mode is already running";
		break;
	case MESH_AUTO_CONFIG_TEST_RESTART:
		refused = "mesh11sd is running with auto_config on, restart mesh11sd first";
		break;
	default:
		if (mesh_auto_config_test_start() != 0) {
			ubus_log_err("unable to start the auto_config test");
			return UBUS_STATUS_UNKNOWN_ERROR;
		}
	}
	blob_buf_init(&bb, 0);
	blobmsg_add_u8(&bb, "started", refused == NULL);
	if (refused)
		blobmsg_add_string(&bb, "message", refused);
	ubus_send_reply(ctx, req, bb.head);
	return 0;
}

static int ubus_version(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	blob_buf_init(&bb, 0);
	blobmsg_add_string(&bb, "version", MESH11SD_VERSION);
	blobmsg_add_string(&bb, "build_commit", MESH11SD_BUILD_COMMIT);
	ubus_send_reply(ctx, req, bb.head);
	return 0;
}

static const struct ubus_method mesh11sd_methods[] = {
	UBUS_METHOD_NOARG("status", ubus_status),
	UBUS_METHOD_NOARG("stations", ubus_stations),
	UBUS_METHOD_NOARG("config_reload", ubus_config_reload),
	UBUS_METHOD_NOARG("auto_config_test", ubus_auto_config_test),
	UBUS_METHOD_NOARG("version", ubus_version),
};

static struct ubus_object_type mesh11sd_object_type =
	UBUS_OBJECT_TYPE("mesh11sd", mesh11sd_methods);

static struct ubus_object mesh11sd_object = {
	.name = "mesh11sd",
	.type = &mesh11sd_object_type,
	.methods = mesh11sd_methods,
	.n_methods = ARRAY_SIZE(mesh11sd_methods),
};

/* ubusd went away: stop, and procd starts us again */
static void ubus_lost(struct ubus_context *ctx)
{
	(void)ctx;
	ubus_log_err("connection to ubusd lost");
	uloop_end();
}

int mesh_ubus_run(struct mesh_ctx *ctx)
{
	struct ubus_context *ubus;

	mctx = ctx;
	ctx->mute = 1;
	uloop_init();
	ubus = ubus_connect(NULL);
	if (!ubus) {
		ubus_log_err("cannot connect to ubusd");
		return 1;
	}
	ubus->connection_lost = ubus_lost;
	ubus_add_uloop(ubus);
	if (ubus_add_object(ubus, &mesh11sd_object) != 0) {
		ubus_log_err("cannot add the mesh11sd object");
		ubus_free(ubus);
		return 1;
	}
	uloop_run();
	ubus_free(ubus);
	uloop_done();
	blob_buf_free(&bb);
	return 0;	/* stopped by a signal, or ubusd went away (procd respawns) */
}
