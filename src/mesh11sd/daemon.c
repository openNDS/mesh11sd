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
			if (p)
				sscanf(p + 13, "%31[^\"]", ctx->factory_mac);
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
	char *nft_argv[] = { "nft", "destroy", "table", "bridge", "mesh11s", NULL };

	ctx->mute = 0;
	ctx->firstloop = 1;
	ctx->daemon_startup = 1;
	snprintf(ctx->last_state, sizeof(ctx->last_state), "initial");
	mesh_ctx_load_setup(ctx);
	mesh_run_capture(nft_argv, NULL, 0);
	ensure_factory_mac(ctx);
	unlink(MESH11SD_TMPDIR "/last_mac_indexed");
	unlink(MESH11SD_TMPDIR "/maclist");
	unlink(MESH11SD_TMPDIR "/loop_interval");
	unlink(MESH11SD_TMPDIR "/active_mesh_ifname");

	mesh_log_notice(ctx, "mesh11sd is in startup");
	mesh_refresh_bridgemac(ctx);
	{
		char *odhcpd[] = { "/sbin/service", "odhcpd", "stop", NULL };
		mesh_run_capture(odhcpd, NULL, 0);
	}

	if (ctx->auto_config > 0 ||
	    access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0) {
		char setl[192];

		/* v7 autostartup: revert+backup, dhcp defaults, then network apply */
		mesh_backup_defaults(ctx);
		if (ctx->portal_detect == 0 || ctx->portal_detect == 4)
			mesh_set_ula_prefix(ctx, "set");
		if (ctx->portal_detect == 5) {
			snprintf(setl, sizeof(setl),
				 "set dhcp.%s.dhcpv4='disabled'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl),
				 "set dhcp.%s.dhcpv6='disabled'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl),
				 "set dhcp.%s.ra='disabled'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl),
				 "set dhcp.%s.ignore='1'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl),
				 "set network.%s.proto='dhcp'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
		} else if (ctx->portal_detect != 3 && ctx->portal_detect != 4) {
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
		if (has_mesh_uci()) {
			char show[4096];
			char *p, *line;
			mesh_uci_show_filter("wireless", "mode='mesh'", show, sizeof(show));
			p = show;
			while ((line = strsep(&p, " \n")) != NULL) {
				char sect[96], *eq, dis[128], ifn[128];
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
		/* v7 ~11100: network restart, bridge mac, dnsmasq, firewall, probe */
		{
			char *nr[] = { "/sbin/service", "network", "restart", NULL };
			mesh_run_capture(nr, NULL, 0);
		}
		mesh_move_ethernet_to_vxlan_bridge(ctx);
		mesh_set_bridge_mac_addresses(ctx);
		{
			char *dns[] = { "/sbin/service", "dnsmasq", "restart", NULL };
			char *fw[] = { "/sbin/service", "firewall", "restart", NULL };
			mesh_run_capture(dns, NULL, 0);
			mesh_run_capture(fw, NULL, 0);
		}
		sleep(5);
		mesh_probe_interface_mac(ctx);
	}

	mesh_check_config_params(ctx);
	mesh_sys_setup(ctx);
	mesh_get_portal_ula(ctx, "discover");
	mesh_log_notice(ctx, "Starting daemon service...");
	mesh_set_led(ctx, "init");

	for (;;) {
		int loop_interval = ctx->checkinterval;

		mesh_ctx_load_setup(ctx);
		if (access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0 &&
		    ctx->auto_config == 0)
			mesh_log_warn(ctx, "Auto Config is in TEST mode");
		if (!ctx->enabled) {
			sleep((unsigned)ctx->checkinterval);
			continue;
		}

		if (!has_mesh_uci()) {
			mesh_log_info(ctx, "No mesh interfaces detected yet.... Attempting auto configure");
			if (ctx->auto_config != 0 ||
			    access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0) {
				mesh_log_debug(ctx, "Starting auto configure process");
				if (mesh_auto_config(ctx) == 0) {
					ctx->changed = 1;
					/* ensure netifd/dnsmasq/firewall see new wireless (v7 apply sites) */
					{
						char *nr[] = { "/sbin/service", "network", "reload", NULL };
						char *dns[] = { "/sbin/service", "dnsmasq", "restart", NULL };
						char *fw[] = { "/sbin/service", "firewall", "restart", NULL };
						mesh_run_capture(nr, NULL, 0);
						mesh_move_ethernet_to_vxlan_bridge(ctx);
						mesh_set_bridge_mac_addresses(ctx);
						mesh_run_capture(dns, NULL, 0);
						mesh_run_capture(fw, NULL, 0);
					}
					mesh_probe_interface_mac(ctx);
				}
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

		/* portal_detect=0 forced MRP also uses check_portal */
		if (ctx->portal_detect == 0 || ctx->portal_detect == 1 ||
		    ctx->portal_detect == 3 || ctx->portal_detect == 4 ||
		    ctx->portal_detect == 5) {
			if (!ctx->firstloop)
				mesh_check_portal(ctx);
		}

		if (ctx->vtun_enable)
			mesh_check_vtunnel(ctx);

		mesh_get_portal_type(ctx);
		mesh_ccpy_setup(ctx);

		if (ctx->auto_config != 0 ||
		    access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0 ||
		    has_mesh_uci()) {
			mesh_set_bridge_mac_addresses(ctx);
		}

		mesh_move_ethernet_to_vxlan_bridge(ctx);
		mesh_set_root_bridge_priority(ctx);
		mesh_block_bridge_loops(ctx);

		mesh_rootmode(ctx);
		mesh_mobility_settings(ctx, "runtime");
		mesh_check_gate(ctx);
		mesh_check_path_changes(ctx);
		if (ctx->portal_detect != 4)
			mesh_check_channel(ctx);
		mesh_maybe_silent_commit_all(ctx);
		mesh_check_mesh_active_led(ctx);

		if (ctx->firstloop || ctx->changed) {
			ctx->firstloop = 0;
			ctx->changed = 0;
			mesh_set_txpower_uci(ctx);
			mesh_set_bridge_mac_addresses(ctx);
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
