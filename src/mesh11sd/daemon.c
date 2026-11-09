/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Set by SIGTERM/SIGINT so stop can kill us without a further loop pass
 * that might network/dnsmasq reload against a just-reverted stock UCI. */
static volatile sig_atomic_t mesh_daemon_stop;

static void mesh_daemon_on_signal(int sig)
{
	(void)sig;
	mesh_daemon_stop = 1;
}

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
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = mesh_daemon_on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	ctx->mute = 0;
	ctx->firstloop = 1;
	ctx->macfixup = 0;
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
		/* v7 ~10925: deterministic lan IPv4 when portal_use_default_ipv4=0.
		 * All nodes calculate it; portals (proto=static) apply/use it. */
		if (ctx->portal_detect != 5)
			mesh_apply_portal_ipv4(ctx);
		if (ctx->portal_detect != 3) {
			snprintf(setl, sizeof(setl),
				 "set network.%s.ip6assign='64'", ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
		}
		mesh_uci_batch_line("set dhcp.lan.ra_lifetime='3600'");
		mesh_uci_batch_line("set dhcp.lan.ra_mininterval='20'");
		mesh_uci_batch_line("set dhcp.lan.ra_maxinterval='60'");
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
		/* v7 restore_ipv4: ensure calculated portal address is on the bridge */
		if (ctx->portal_detect != 5 && ctx->portal_use_default_ipv4 == 0)
			mesh_restore_ipv4(ctx);
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
	/* v7: set_led on before peers; heartbeat once peers appear */
	mesh_set_led(ctx, "on");

	for (;;) {
		int loop_interval = ctx->checkinterval;
		char buf[64];

		if (mesh_daemon_stop) {
			mesh_log_notice(ctx, "Mesh11sd is shutting down");
			break;
		}

		/* v7 main loop only refreshes debuglevel; full option dump is startup-only */
		if (mesh_uci_get("mesh11sd.setup.debuglevel", buf, sizeof(buf)) == 0) {
			int dl = atoi(buf);
			if (dl >= 0 && dl <= 3)
				ctx->debuglevel = dl;
		}
		if (mesh_uci_get("mesh11sd.setup.enabled", buf, sizeof(buf)) == 0)
			ctx->enabled = atoi(buf) ? 1 : 0;
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
					/* Re-assert portal IPv4 after auto_config (covers test-mode
					 * path where startup skipped the autostartup block). */
					if (ctx->portal_detect != 5)
						mesh_apply_portal_ipv4(ctx);
					/* ensure netifd/dnsmasq/firewall see new wireless (v7 apply sites) */
					{
						char *nr[] = { "/sbin/service", "network", "reload", NULL };
						char *dns[] = { "/sbin/service", "dnsmasq", "restart", NULL };
						char *fw[] = { "/sbin/service", "firewall", "restart", NULL };
						mesh_run_capture(nr, NULL, 0);
						mesh_move_ethernet_to_vxlan_bridge(ctx);
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
		}

		if (has_mesh_uci()) {
			/* v7 ~11225: UCI macaddr before wifi so runtime MAC walk is a no-op */
			mesh_fixup_wireless_macaddrs(ctx);
		}

		/* v7: firstloop/changed → all_nodes_rssi_update → restart_mesh (wifi).
		 * Previously only wifi'd when changed; firstloop alone skipped wifi so
		 * 5g APs could stay half-up (beacon/TX ok, rx_packets=0, no DHCP) after
		 * service restart until a manual `wifi`. */
		if (ctx->firstloop || ctx->changed) {
			ctx->firstloop = 0;
			ctx->changed = 0;
			mesh_set_txpower_uci(ctx);
			mesh_file_write(MESH11SD_TMPDIR "/meshinterface", "");
			unlink(MESH11SD_TMPDIR "/meshinterface");
			mesh_restart_mesh(ctx);
		}

		if (has_mesh_uci()) {
			mesh_get_mesh_iflist(ctx);
			mesh_apply_mesh_params(ctx);
			/* v7: check_mesh_phantom after check_mesh_params per mesh iface */
			mesh_check_mesh_phantom(ctx);
		}

		/* portal_detect=0 forced MRP also uses check_portal */
		if (ctx->portal_detect == 0 || ctx->portal_detect == 1 ||
		    ctx->portal_detect == 3 || ctx->portal_detect == 4 ||
		    ctx->portal_detect == 5) {
			if (!ctx->firstloop)
				mesh_check_portal(ctx);
		}

		if (ctx->vtun_enable) {
			mesh_check_vtunnel(ctx);
			/* v7: update_vxlan_for_active_portal after check_vtunnel path */
			mesh_update_vxlan_for_active_portal(ctx);
		}

		mesh_get_portal_type(ctx);
		mesh_ccpy_setup(ctx);

		/* v7: manage_fallback_ula_route when portal_detect 4|5 every loop */
		if (ctx->portal_detect == 4 || ctx->portal_detect == 5)
			mesh_manage_fallback_ula_route(ctx);

		if (ctx->auto_config != 0 ||
		    access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0 ||
		    has_mesh_uci()) {
			mesh_set_bridge_mac_addresses(ctx);
		}

		mesh_move_ethernet_to_vxlan_bridge(ctx);
		mesh_set_root_bridge_priority(ctx);

		/* v7: portal nodes also run manage_fallback_ula_route every loop */
		if (!strcmp(ctx->detected_state, "portal")) {
			mesh_manage_fallback_ula_route(ctx);
			/* Heal: force=1 so dnsmasq init keeps lan/vtun ranges when
			 * another portal answers dhcp_check (see portal_handoff). */
			mesh_ensure_portal_dhcp_force(ctx);
		}

		/* v7 ~11457: mesh_mac_forced_forwarding + gateway_proxy_arp each loop */
		{
			FILE *f;
			char path[192];
			const char *meshif = ctx->active_mesh_ifname;
			const char *br = ctx->device[0] ? ctx->device : NULL;
			if (meshif[0]) {
				snprintf(path, sizeof(path),
					 "/proc/sys/net/ipv4/conf/%s/proxy_arp_pvlan", meshif);
				f = fopen(path, "w");
				if (f) {
					fputs(ctx->mesh_mac_forced_forwarding ? "1\n" : "0\n", f);
					fclose(f);
				}
			}
			if (br && br[0]) {
				snprintf(path, sizeof(path),
					 "/proc/sys/net/ipv4/conf/%s/proxy_arp", br);
				f = fopen(path, "w");
				if (f) {
					fputs(ctx->gateway_proxy_arp ? "1\n" : "0\n", f);
					fclose(f);
				}
			}
		}

		mesh_block_bridge_loops(ctx);
		mesh_pin_active_portal_arp(ctx);

		mesh_rootmode(ctx);
		mesh_mobility_settings(ctx, "runtime");
		mesh_check_gate(ctx);
		mesh_check_path_changes(ctx);
		mesh_check_odhcpd(ctx);
		if (ctx->portal_detect != 4)
			mesh_check_channel(ctx);
		mesh_maybe_silent_commit_all(ctx);
		mesh_check_mesh_active_led(ctx);
		/* v7 end-of-loop MAC walk */
		mesh_set_bridge_mac_addresses(ctx);

		{
			char content[64];
			snprintf(content, sizeof(content), "loop_interval=\"%d\"\n", loop_interval);
			mesh_file_write(MESH11SD_TMPDIR "/loop_interval", content);
		}
		mesh_log_info(ctx, "Checkinterval [%d]", loop_interval);
		sleep((unsigned)loop_interval);
		if (mesh_daemon_stop) {
			mesh_log_notice(ctx, "Mesh11sd is shutting down");
			break;
		}
	}
	return 0;
}
