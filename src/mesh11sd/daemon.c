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
	int reqs_ok;

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

	/* Check ip-full and a mesh-capable wpad first: a node missing them
	 * must be left exactly as it is (no MACs, ULA or IPv4 staged). The
	 * main loop keeps checking and logs what is missing. */
	reqs_ok = mesh_check_requirements(ctx) == 0;

	mesh_refresh_bridgemac(ctx);
	if (reqs_ok) {
		char *odhcpd[] = { "/sbin/service", "odhcpd", "stop", NULL };
		mesh_run_capture(odhcpd, NULL, 0);
	}

	/* Backups for revert_all, taken on any start if missing */
	mesh_backup_configs(ctx);

	if (reqs_ok && (ctx->auto_config > 0 ||
	    access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0)) {
		/* v7 autostartup: revert+backup, dhcp defaults, then network apply */
		mesh_backup_defaults(ctx);
		/* v7 clean sheet: no committed MACs from an earlier run */
		mesh_clear_own_macaddrs(ctx);
		mesh_apply_autostartup_uci(ctx);
		if (has_mesh_uci()) {
			char show[4096];
			char *p, *line;
			mesh_uci_show_filter("wireless", "mode='mesh'", show, sizeof(show));
			p = show;
			while ((line = strsep(&p, " \n")) != NULL) {
				char sect[96], *eq, dis[128], ifn[128];
				/* line is "wireless.<section>.mode='mesh'"; keep "wireless.<section>" */
				eq = strstr(line, ".mode=");
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
		/* Stage the walk MACs in the wireless UCI too, so the restart
		 * brings the vifs up on them and the live MAC walk after it
		 * changes nothing (ath11k loses 5g beacons when an AP MAC moves
		 * under a running hostapd). The first loop then has no reload. */
		if (has_mesh_uci()) {
			mesh_reserve_bridge_macs(ctx);
			mesh_fixup_wireless_macaddrs(ctx);
		}
		/* portal-side network UCI first, so this one restart covers it */
		mesh_network_preconfigure(ctx);
		/* modes 4|5: wan joins the trunk in this same restart */
		mesh_ensure_wan_on_vxlan_bridge(ctx, 0);
		/* v7 ~11100: network restart, bridge mac, dnsmasq, firewall, probe */
		{
			char *nr[] = { "/sbin/service", "network", "restart", NULL };
			mesh_run_capture(nr, NULL, 0);
			mesh_network_mark_applied();
		}
		/* CPE: br-wan exists only after the restart; use it from now on */
		if (ctx->portal_detect == 3)
			mesh_refresh_bridgemac(ctx);
		/* v7 restore_ipv4: ensure calculated portal address is on the bridge.
		 * Not on a CPE: its mesh bridge is wan, which uses DHCP. */
		if (ctx->portal_detect != 3 && ctx->portal_detect != 5 &&
		    ctx->portal_use_default_ipv4 == 0 && !mesh_predicted_peer(ctx))
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

	/* v7 ~11124: drop the previous-run rssi lock before the main loop */
	unlink(MESH11SD_TMPDIR "/mesh_rssi_threshold");

	mesh_check_config_params(ctx);
	if (reqs_ok) {
		mesh_sys_setup(ctx);
		mesh_get_portal_ula(ctx, "discover");
	}
	mesh_log_notice(ctx, "Starting daemon service...");
	if (reqs_ok) {
		/* Wait for boot-netifd vifs before any phy-wide writes. */
		mesh_wait_for_wifi_enabled(ctx);
		/* v7 mobility_settings: phy-wide values with iw only, no wifi */
		mesh_mobility_apply_phywide(ctx);
	}
	mesh_set_led(ctx, "init");
	/* v7: set_led on before peers; heartbeat once peers appear */
	mesh_set_led(ctx, "on");

	for (;;) {
		int loop_interval = ctx->checkinterval;
		int auto_config_done;
		char buf[64];

		ctx->loop_seq++;
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

		/* Block auto_config (and further config this pass) until
		 * requirements are met: no ath10k-ct, ip-full, mesh wpad. */
		if (mesh_check_requirements(ctx) != 0) {
			sleep((unsigned)ctx->checkinterval);
			continue;
		}

		auto_config_done = 0;
		if (!has_mesh_uci()) {
			mesh_log_info(ctx, "No mesh interfaces detected yet.... Attempting auto configure");
			if (ctx->auto_config != 0 ||
			    access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0) {
				mesh_log_debug(ctx, "Starting auto configure process");
				if (mesh_auto_config(ctx) == 0) {
					ctx->changed = 1;
					auto_config_done = 1;
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
						mesh_network_mark_applied();
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
			if (mesh_fixup_wireless_macaddrs(ctx) > 0 && ctx->firstloop) {
				/* hostapd started on the old MACs; one wifi reload
				 * brings the vifs up on the walk MACs */
				char *wr[] = { "/sbin/wifi", "reload", NULL };

				mesh_log_notice(ctx, "wireless MAC addresses changed, reloading wifi");
				mesh_run_capture(wr, NULL, 0);
			}
		}

		/* auto_config=1, portal_detect 0/1: commit the portal config
		 * before detection, keep LuCI usable, and put back managed
		 * options changed outside mesh11sd (docs/LUCI-DESIGN.md). */
		if (has_mesh_uci() && mesh_managed_mode(ctx))
			mesh_managed_pass(ctx, auto_config_done);

		/* v7 ~11271: firstloop/changed only touches meshinterface and calls
		 * all_nodes_rssi_update. That runs restart_mesh (wifi) only if
		 * meshinterface exists and mesh_rssi_threshold does not — not on
		 * every firstloop. Alpha19 wifid unconditionally, which breaks
		 * 5g on mt7996e when the mesh is already up. */
		if (ctx->firstloop || ctx->changed) {
			ctx->firstloop = 0;
			ctx->changed = 0;
			mesh_set_led(ctx, "on");
			/* Startup removes the rssi lock (v7 ~11124), so a live,
			 * already-correct mesh would still wifi here. If a mesh
			 * iface is up with the configured threshold, restore the
			 * lock so a normal start does not run wifi. */
			if (access(MESH11SD_TMPDIR "/mesh_rssi_threshold", F_OK) != 0) {
				char conf[64];
				int i;

				if (mesh_uci_get("mesh11sd.mesh_params.mesh_rssi_threshold",
						 conf, sizeof(conf)) != 0 || !conf[0])
					snprintf(conf, sizeof(conf), "%d",
						 ctx->mesh_rssi_threshold);
				mesh_get_mesh_iflist(ctx);
				for (i = 0; i < ctx->iflist_count; i++) {
					char live[64] = "";
					char *argv[] = {
						"iw", "dev", ctx->iflist[i], "get",
						"mesh_param", "mesh_rssi_threshold",
						NULL
					};
					if (mesh_run_capture(argv, live, sizeof(live)) != 0)
						continue;
					if (atoi(live) == atoi(conf)) {
						mesh_file_write(MESH11SD_TMPDIR
								"/mesh_rssi_threshold",
								conf);
						break;
					}
				}
			}
			/* v7: touch "$tmpdir/meshinterface" */
			mesh_file_write(MESH11SD_TMPDIR "/meshinterface", "");
			mesh_all_nodes_rssi_update(ctx);
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
		if (ctx->portal_detect == 3 &&
		    (ctx->auto_config != 0 ||
		     access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0))
			mesh_cpe_conf(ctx);

		if (ctx->vtun_enable) {
			mesh_check_vtunnel(ctx);
			/* v7: update_vxlan_for_active_portal after check_vtunnel path */
			mesh_update_vxlan_for_active_portal(ctx);
		}

		mesh_get_portal_type(ctx);
		mesh_ccpy_setup(ctx);

		/* v7: manage_fallback_ula_route when portal_detect 4|5 every loop */
		if (ctx->portal_detect == 4 || ctx->portal_detect == 5) {
			mesh_manage_fallback_ula_route(ctx);
			mesh_ensure_wan_on_vxlan_bridge(ctx, 1);
		}

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
			mesh_ensure_portal_vtun_dnsmasq(ctx);
		}

		/* v7 ~11457: mesh_mac_forced_forwarding + gateway_proxy_arp each loop */
		{
			char path[192];
			const char *meshif = ctx->active_mesh_ifname;
			const char *br = ctx->device[0] ? ctx->device : NULL;
			int want, cur;

			if (meshif[0]) {
				want = ctx->mesh_mac_forced_forwarding ? 1 : 0;
				snprintf(path, sizeof(path),
					 "/proc/sys/net/ipv4/conf/%s/proxy_arp_pvlan", meshif);
				cur = -1;
				mesh_sysfs_read_int(path, &cur);
				if (mesh_sysfs_write_int_if_changed(path, want) == 1)
					mesh_log_info(ctx,
						      "Old value:%s proxy_arp_pvlan=%d, Setting new value:proxy_arp_pvlan=%d",
						      meshif, cur, want);
			}
			if (br && br[0]) {
				want = ctx->gateway_proxy_arp ? 1 : 0;
				snprintf(path, sizeof(path),
					 "/proc/sys/net/ipv4/conf/%s/proxy_arp", br);
				cur = -1;
				mesh_sysfs_read_int(path, &cur);
				if (mesh_sysfs_write_int_if_changed(path, want) == 1)
					mesh_log_info(ctx,
						      "Old value:%s proxy_arp=%d, Setting new value:proxy_arp=%d",
						      br, cur, want);
			}
		}

		mesh_block_bridge_loops(ctx);
		mesh_pin_active_portal_arp(ctx);
		mesh_cpe_discovery_rule(ctx);

		mesh_rootmode(ctx);
		mesh_mobility_apply_phywide(ctx);
		mesh_mobility_settings(ctx, "runtime");
		mesh_fix_mesh_noscan(ctx);
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
