/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int has_mesh_uci(void)
{
	char show[4096];

	if (mesh_uci_show_filter("wireless", "mode='mesh'", show, sizeof(show)) != 0)
		return 0;
	return show[0] != '\0';
}

static void ensure_factory_mac(struct mesh_ctx *ctx)
{
	char path[128];
	char content[128];

	snprintf(path, sizeof(path), "%s/factory_mac", MESH11SD_TMPDIR);
	if (access(path, R_OK) == 0) {
		FILE *f = fopen(path, "r");
		char line[128];
		if (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "factory_mac=\"");
			if (p) {
				p += 13;
				sscanf(p, "%31[^\"]", ctx->factory_mac);
			}
		}
		if (f)
			fclose(f);
	}
	if (!ctx->factory_mac[0]) {
		if (mesh_get_factory_mac(ctx, ctx->factory_mac, sizeof(ctx->factory_mac)) == 0) {
			snprintf(content, sizeof(content),
				 "factory_mac=\"%s\"; installver=\"%s\"\n",
				 ctx->factory_mac, MESH11SD_VERSION);
			mesh_file_write(path, content);
		}
	}
}

int mesh_daemon_run(struct mesh_ctx *ctx)
{
	int firstloop = 1;
	int changed = 0;
	char *nft_argv[] = { "nft", "destroy", "table", "bridge", "mesh11s", NULL };

	ctx->mute = 0;
	mesh_ctx_load_setup(ctx);
	mesh_run_capture(nft_argv, NULL, 0);
	ensure_factory_mac(ctx);
	unlink(MESH11SD_TMPDIR "/last_mac_indexed");
	unlink(MESH11SD_TMPDIR "/maclist");
	unlink(MESH11SD_TMPDIR "/loop_interval");

	mesh_log_notice(ctx, "mesh11sd is in startup");
	mesh_refresh_bridgemac(ctx);
	{
		char *odhcpd[] = { "/sbin/service", "odhcpd", "stop", NULL };
		mesh_run_capture(odhcpd, NULL, 0);
	}

	if (ctx->auto_config > 0 ||
	    access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0) {
		mesh_backup_defaults(ctx);
		/* basic portal dhcp defaults for portal_detect 0/1 */
		if (ctx->portal_detect != 3 && ctx->portal_detect != 4 &&
		    ctx->portal_detect != 5) {
			char setl[160];
			snprintf(setl, sizeof(setl),
				 "set dhcp.%s.dhcpv4='server'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl),
				 "set dhcp.%s.dhcpv6='server'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl),
				 "set dhcp.%s.ra='server'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl),
				 "set dhcp.%s.ignore='0'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl),
				 "set network.%s.proto='static'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
		}
		/* pick existing active mesh ifname if present */
		if (has_mesh_uci()) {
			char show[4096];
			char *p, *line;
			mesh_uci_show_filter("wireless", "mode='mesh'", show, sizeof(show));
			p = show;
			while ((line = strsep(&p, " \n")) != NULL) {
				char sect[96], *eq, dis[8], ifn[64];
				eq = strchr(line, '=');
				if (!eq)
					continue;
				snprintf(sect, sizeof(sect), "%s", line);
				sect[eq - line] = '\0';
				snprintf(dis, sizeof(dis), "%s.disabled", sect);
				if (mesh_uci_get(dis, dis, sizeof(dis)) == 0 && atoi(dis) == 0) {
					snprintf(ifn, sizeof(ifn), "%s.ifname", sect);
					if (mesh_uci_get(ifn, ctx->active_mesh_ifname,
							 sizeof(ctx->active_mesh_ifname)) == 0) {
						char content[128];
						snprintf(content, sizeof(content),
							 "active_mesh_ifname=\"%s\"\n",
							 ctx->active_mesh_ifname);
						mesh_file_write(MESH11SD_TMPDIR "/active_mesh_ifname",
								content);
						break;
					}
				}
			}
		}
	}

	mesh_check_config_params(ctx);
	mesh_sys_setup(ctx);
	mesh_log_notice(ctx, "Starting daemon service...");

	for (;;) {
		int loop_interval = ctx->checkinterval;

		mesh_ctx_load_setup(ctx);
		if (!ctx->enabled) {
			sleep((unsigned)ctx->checkinterval);
			continue;
		}

		if (!has_mesh_uci()) {
			mesh_log_info(ctx, "No mesh interfaces detected yet.... Attempting auto configure");
			if (ctx->auto_config != 0 ||
			    access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0) {
				mesh_log_debug(ctx, "Starting auto configure process");
				if (mesh_auto_config(ctx) == 0)
					changed = 1;
			} else {
				mesh_log_err(ctx, "auto_config is disabled. Please configure a mesh interface or enable auto_config....");
				mesh_log_info(ctx, "Checkinterval [%d]", ctx->checkinterval);
				sleep((unsigned)ctx->checkinterval);
				continue;
			}
		} else {
			mesh_get_mesh_iflist(ctx);
			mesh_apply_mesh_params(ctx);
		}

		if (firstloop || changed) {
			firstloop = 0;
			changed = 0;
			mesh_set_txpower_uci(ctx);
		}

		{
			char content[64];
			snprintf(content, sizeof(content), "loop_interval=\"%d\"\n", loop_interval);
			mesh_file_write(MESH11SD_TMPDIR "/loop_interval", content);
		}
		mesh_log_info(ctx, "Checkinterval [%d]", loop_interval);
		sleep((unsigned)loop_interval);
	}
	return 0;
}
