/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 5: portal detect, peer/portal handoff, watchdog, ULA (v7-faithful).
 * Peer handoff sets network.<zone>.proto='dhcp' (not none), as v7.
 */
#include "mesh11sd.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_is_portal(struct mesh_ctx *ctx)
{
	char content[160];

	snprintf(content, sizeof(content),
		 "is_portal=\"%s\"; detected_state=\"%s\"\n",
		 ctx->is_portal, ctx->detected_state);
	mesh_file_write(MESH11SD_TMPDIR "/is_portal", content);
}

static int iface_is_up(const char *ifname)
{
	char out[512];
	char *argv[] = { "ip", "link", "show", (char *)ifname, NULL };

	if (!ifname[0] || mesh_run_capture(argv, out, sizeof(out)) != 0)
		return 0;
	return strstr(out, "state UP") || strstr(out, "state UNKNOWN");
}

static void service_cmd(const char *svc, const char *action)
{
	char *argv[] = { "/sbin/service", (char *)svc, (char *)action, NULL };
	mesh_run_capture(argv, NULL, 0);
}

int mesh_set_ula_prefix(struct mesh_ctx *ctx, const char *mode)
{
	char factory_hash[72];
	char current[64] = "";
	char preconf[64] = "";
	char setl[160];
	const char *id = ctx->auto_mesh_id;

	if (!mode || !mode[0])
		return 1;
	if (!ctx->factory_mac[0])
		mesh_get_factory_mac(ctx, ctx->factory_mac, sizeof(ctx->factory_mac));
	/* v7: factory_hash=$(echo "$factory_mac" | sha256sum) — newline in digest */
	if (mesh_sha256_hex_echo(ctx->factory_mac, factory_hash, sizeof(factory_hash)) != 0)
		snprintf(factory_hash, sizeof(factory_hash), "%s", ctx->factory_mac);

	mesh_uci_get("network.globals.ula_prefix", current, sizeof(current));

	if (!strcmp(mode, "revert")) {
		/* preconf from backup file if present */
		FILE *f = fopen("/etc/config/network_mesh11sd_default", "r");
		if (!f)
			f = fopen("/etc/config/network", "r");
		if (f) {
			char line[256];
			while (fgets(line, sizeof(line), f)) {
				char *p = strstr(line, "ula_prefix");
				if (!p)
					continue;
				p = strchr(line, '\'');
				if (p) {
					char *e = strchr(p + 1, '\'');
					if (e) {
						*e = '\0';
						snprintf(preconf, sizeof(preconf), "%s", p + 1);
					}
				}
				break;
			}
			fclose(f);
		}
		snprintf(ctx->ula_prefix, sizeof(ctx->ula_prefix), "%s", preconf);
	} else if (!strcmp(mode, "set") || !strcmp(mode, "get")) {
		char first[4], second[8], third[8];
		/* v7: auto_mesh_id:9:2, :11:4, :15:4 (0-based offsets in ash are 1-based substr) */
		/* ash ${var:9:2} is 0-based from offset 9 */
		size_t n = strlen(id);
		snprintf(first, sizeof(first), "%.2s", n > 9 ? id + 9 : "00");
		snprintf(second, sizeof(second), "%.4s", n > 11 ? id + 11 : "0000");
		snprintf(third, sizeof(third), "%.4s", n > 15 ? id + 15 : "0000");
		snprintf(ctx->ula_subnet, sizeof(ctx->ula_subnet), "%.4s",
			 strlen(factory_hash) > 24 ? factory_hash + 20 : "0000");
		snprintf(ctx->ula_vxsubnet, sizeof(ctx->ula_vxsubnet), "%.4s",
			 strlen(factory_hash) > 29 ? factory_hash + 25 : "0000");
		snprintf(ctx->ula_base, sizeof(ctx->ula_base), "fd%s:%s:%s", first, second, third);
		snprintf(ctx->ula_prefix, sizeof(ctx->ula_prefix), "%s::/48", ctx->ula_base);
	} else {
		return 1;
	}

	if (strcmp(mode, "get") != 0 &&
	    (!current[0] || strcmp(current, ctx->ula_prefix) != 0)) {
		snprintf(setl, sizeof(setl), "set network.globals.ula_prefix='%s'",
			 ctx->ula_prefix);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set network.%s.ip6hint='%s'",
			 ctx->auto_mesh_network, ctx->ula_subnet);
		mesh_uci_batch_line(setl);
		mesh_log_debug(ctx, "ULA prefix set to %s (LAN hint: %s, VXLAN hint: %s)",
			       ctx->ula_prefix, ctx->ula_subnet, ctx->ula_vxsubnet);
	}
	return 0;
}

static int ping_portal(struct mesh_ctx *ctx, const char *addr)
{
	char *argv[] = { "ping", "-W", "20", "-c", "1", (char *)addr, NULL };
	(void)ctx;
	return mesh_run_capture(argv, NULL, 0) == 0 ? 0 : 1;
}

int mesh_get_portal_ula(struct mesh_ctx *ctx, const char *mode)
{
	char m[16];

	mesh_set_ula_prefix(ctx, "get");
	snprintf(m, sizeof(m), "%s", mode && mode[0] ? mode : "get");

	if (!strcmp(ctx->detected_state, "portal")) {
		snprintf(ctx->portal_ula, sizeof(ctx->portal_ula), "%s:%s::1",
			 ctx->ula_base, ctx->ula_subnet);
		ctx->link_to_portal = 0;
		ctx->portal_metric = 0;
		ctx->portal_hops = 0;
		{
			char content[192];
			snprintf(content, sizeof(content),
				 "portal_ula=\"%s\"; ula_subnet=\"%s\"; portal_mac=\"%s\"; metric=0; hops=0\n",
				 ctx->portal_ula, ctx->ula_subnet, ctx->factory_mac);
			mesh_file_write(MESH11SD_TMPDIR "/active_portal", content);
		}
		return 0;
	}

	if (!strcmp(m, "get") && access(MESH11SD_TMPDIR "/active_portal", R_OK) == 0) {
		FILE *f = fopen(MESH11SD_TMPDIR "/active_portal", "r");
		char line[256];
		if (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "portal_ula=\"");
			if (p)
				sscanf(p + 12, "%63[^\"]", ctx->portal_ula);
			p = strstr(line, "metric=");
			if (p)
				ctx->portal_metric = atoi(p + 7);
			p = strstr(line, "hops=");
			if (p)
				ctx->portal_hops = atoi(p + 5);
			p = strstr(line, "portal_mac=\"");
			if (p)
				sscanf(p + 12, "%31[^\"]", ctx->portal_mac);
			ctx->link_to_portal = ping_portal(ctx, ctx->portal_ula);
			if (ctx->link_to_portal == 0) {
				fclose(f);
				return 0;
			}
			snprintf(m, sizeof(m), "discover");
		}
		if (f)
			fclose(f);
		else
			snprintf(m, sizeof(m), "discover");
	} else if (!strcmp(m, "get")) {
		snprintf(m, sizeof(m), "discover");
	}

	if (!strcmp(m, "discover"))
		return mesh_portal_ula_discover(ctx);
	return 1;
}

int mesh_get_portal_state(struct mesh_ctx *ctx)
{
	char up_zone[64] = "";
	char default_ipv6[256] = "";
	char default_ipv4[64] = "";
	char out[4096];

	if (ctx->firstloop)
		return 0;

	mesh_log_debug(ctx, "In get_portal_state. firstloop=%d...", ctx->firstloop);
	ctx->is_portal[0] = '\0';

	if (ctx->portal_detect == 0) {
		snprintf(ctx->is_portal, sizeof(ctx->is_portal), "forced");
		snprintf(ctx->detected_state, sizeof(ctx->detected_state), "portal");
		write_is_portal(ctx);
		return 0;
	}
	if (ctx->portal_detect == 3) {
		snprintf(ctx->detected_state, sizeof(ctx->detected_state), "peer");
		mesh_log_debug(ctx, "CPE mode...");
		write_is_portal(ctx);
		return 0;
	}
	if (ctx->portal_detect == 4) {
		/* MBP: ethernet upstream via detect_mbp_upstream (v7) */
		if (mesh_detect_mbp_upstream(ctx) == 0) {
			snprintf(ctx->is_portal, sizeof(ctx->is_portal), "detected");
			snprintf(ctx->detected_state, sizeof(ctx->detected_state), "portal");
			mesh_log_debug(ctx, "Node has upstream gateway(s), zone [ %s ], [ %s ], [ %s ]",
				       ctx->up_zone, ctx->default_ipv6_gw, ctx->default_ipv4_gw);
		} else {
			ctx->is_portal[0] = '\0';
			snprintf(ctx->detected_state, sizeof(ctx->detected_state), "peer");
		}
		write_is_portal(ctx);
		return 0;
	}
	if (ctx->portal_detect == 5) {
		snprintf(ctx->detected_state, sizeof(ctx->detected_state), "peer");
		write_is_portal(ctx);
		return 0;
	}

	/* portal_detect 1 (auto) */
	mesh_uci_get("network.wan.device", up_zone, sizeof(up_zone));
	if (up_zone[0] && iface_is_up(up_zone)) {
		char *argv6[] = { "ip", "-6", "neigh", NULL };
		char *argv4[] = { "ip", "-4", "neigh", NULL };
		if (mesh_run_capture(argv6, out, sizeof(out)) == 0) {
			char *p = out, *line;
			while ((line = strsep(&p, "\n")) != NULL) {
				if (!strstr(line, up_zone) || !strstr(line, "router"))
					continue;
				if (!strncmp(line, "fe80:", 5))
					continue;
				sscanf(line, "%255s", default_ipv6);
				break;
			}
		}
		if (mesh_run_capture(argv4, out, sizeof(out)) == 0) {
			char *p = out, *line;
			while ((line = strsep(&p, "\n")) != NULL) {
				if (!strstr(line, up_zone))
					continue;
				sscanf(line, "%63s", default_ipv4);
				break;
			}
		}
	}

	if (default_ipv6[0] || default_ipv4[0]) {
		snprintf(ctx->is_portal, sizeof(ctx->is_portal), "detected");
		snprintf(ctx->detected_state, sizeof(ctx->detected_state), "portal");
		mesh_log_debug(ctx, "Node has upstream gateway(s), zone [ %s ]", up_zone);
	} else {
		/* look on backhaul */
		char br_zone[64];
		char key[96];
		int retry;

		snprintf(key, sizeof(key), "network.%s.device", ctx->auto_mesh_network);
		if (mesh_uci_get(key, br_zone, sizeof(br_zone)) != 0 || !br_zone[0])
			snprintf(br_zone, sizeof(br_zone), "br-%s", ctx->auto_mesh_network);
		ctx->is_portal[0] = '\0';
		for (retry = 1; retry <= 4; retry++) {
			char status[16];
			mesh_log_debug(ctx, "Portal upstream link check  [%s] - iteration [ %d ] ....",
				       br_zone, retry - 1);
			mesh_wait_for_interface(ctx, br_zone, status, sizeof(status));
			if (!strcmp(status, "up")) {
				char *argv[] = { "ip", "-6", "neigh", NULL };
				if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
					char *p = out, *line;
					while ((line = strsep(&p, "\n")) != NULL) {
						char addr[64];
						if (!strstr(line, br_zone) || !strstr(line, "router"))
							continue;
						if (!strncmp(line, "fe80:", 5))
							continue;
						sscanf(line, "%63s", addr);
						snprintf(default_ipv6, sizeof(default_ipv6), "%s", addr);
						break;
					}
				}
				if (!default_ipv6[0])
					mesh_get_portal_ula(ctx, "discover");
				if (default_ipv6[0] || (ctx->portal_ula[0] && ctx->link_to_portal == 0)) {
					snprintf(ctx->is_portal, sizeof(ctx->is_portal), "backhaul");
					snprintf(ctx->detected_state, sizeof(ctx->detected_state), "peer");
					break;
				}
			}
		}
		if (!ctx->detected_state[0] ||
		    (!ctx->is_portal[0] && strcmp(ctx->detected_state, "portal") != 0))
			snprintf(ctx->detected_state, sizeof(ctx->detected_state), "peer");
	}

	write_is_portal(ctx);
	return 0;
}

int mesh_restore_ipv4(struct mesh_ctx *ctx)
{
	char key[96], config_ip[64], config_mask[64], out[2048];
	char *slash;
	char *argv[] = { "ip", "-4", "addr", "show", "dev", ctx->device, NULL };

	snprintf(key, sizeof(key), "network.%s.ipaddr", ctx->auto_mesh_network);
	if (mesh_uci_get(key, config_ip, sizeof(config_ip)) != 0 || !config_ip[0])
		return 0;
	/* stock list form may be 192.168.1.1/24 */
	slash = strchr(config_ip, '/');
	if (slash)
		*slash = '\0';
	snprintf(key, sizeof(key), "network.%s.netmask", ctx->auto_mesh_network);
	if (mesh_uci_get(key, config_mask, sizeof(config_mask)) != 0)
		snprintf(config_mask, sizeof(config_mask), "255.255.255.0");

	/* Drop any other global IPv4 on the bridge (e.g. stale .102 from a
	 * prior non-idempotent generate) so we replace rather than stack. */
	if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
		char *p = out, *line;
		while ((line = strsep(&p, "\n")) != NULL) {
			char ip[64], pfx[16];
			const char *inet;
			char *sp;
			if (!(inet = strstr(line, "inet ")))
				continue;
			inet += 5;
			while (*inet == ' ')
				inet++;
			snprintf(ip, sizeof(ip), "%s", inet);
			sp = strchr(ip, ' ');
			if (sp)
				*sp = '\0';
			sp = strchr(ip, '/');
			if (sp) {
				snprintf(pfx, sizeof(pfx), "%s", sp + 1);
				*sp = '\0';
			} else {
				pfx[0] = '\0';
			}
			if (!strcmp(ip, config_ip))
				continue;
			{
				char cidr[80];
				char *del[] = { "ip", "addr", "del", cidr, "dev",
						ctx->device, NULL };
				if (pfx[0])
					snprintf(cidr, sizeof(cidr), "%s/%s", ip, pfx);
				else
					snprintf(cidr, sizeof(cidr), "%s", ip);
				mesh_run_capture(del, NULL, 0);
			}
		}
	}
	/* Re-read: if config_ip is present we are done (v7 no-op path) */
	if (mesh_run_capture(argv, out, sizeof(out)) == 0 && strstr(out, config_ip))
		return 0;
	{
		char cidr[80];
		int prefix = 24;
		if (!strcmp(config_mask, "255.255.255.0"))
			prefix = 24;
		else if (!strcmp(config_mask, "255.255.0.0"))
			prefix = 16;
		snprintf(cidr, sizeof(cidr), "%s/%d", config_ip, prefix);
		{
			char *a[] = { "ip", "addr", "add", cidr, "dev", ctx->device, NULL };
			mesh_run_capture(a, NULL, 0);
		}
		{
			char *a[] = { "ip", "link", "set", ctx->device, "arp", "on", NULL };
			mesh_run_capture(a, NULL, 0);
		}
		{
			char *a[] = { "ip", "link", "set", ctx->device, "multicast", "on", NULL };
			mesh_run_capture(a, NULL, 0);
		}
	}
	return 0;
}

static void peer_handoff(struct mesh_ctx *ctx)
{
	char setl[192];
	char path[128];
	FILE *f;

	mesh_log_debug(ctx, "This meshnode is NOT a portal");
	snprintf(path, sizeof(path), "/var/run/udhcpc-%s.pid", ctx->device);
	if ((f = fopen(path, "r"))) {
		int pid = 0;
		if (fscanf(f, "%d", &pid) == 1 && pid > 0)
			kill(pid, SIGTERM);
		fclose(f);
	}
	{
		char *argv[] = { "ip", "route", "flush", "default", NULL };
		mesh_run_capture(argv, NULL, 0);
	}

	/* v7: if vxlan is up, mark vtunlan for dhcp and tear it down on peer flip */
	{
		char vxname[32], out[128];
		char *show[] = { "ip", "link", "show", vxname, NULL };
		snprintf(vxname, sizeof(vxname), "vxlan%s",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		show[3] = vxname;
		if (mesh_run_capture(show, out, sizeof(out)) == 0) {
			mesh_uci_batch_line("set network.vtunlan.proto='dhcp'");
			{
				char *d[] = { "ip", "link", "set", vxname, "down", NULL };
				mesh_run_capture(d, NULL, 0);
			}
			{
				char *d[] = { "ip", "link", "del", vxname, NULL };
				mesh_run_capture(d, NULL, 0);
			}
		}
	}

	/* v7: proto dhcp — never none */
	snprintf(setl, sizeof(setl), "set network.%s.proto='dhcp'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);

	if (!ctx->portal_ula[0])
		mesh_get_portal_ula(ctx, "get");

	mesh_uci_batch_line("set dhcp.@dnsmasq[0].authoritative='0'");
	snprintf(setl, sizeof(setl), "set dhcp.%s.ignore='1'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl), "set dhcp.%s.ra_default='2'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl), "set dhcp.%s.dhcpv6='disabled'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl), "set dhcp.%s.dhcpv4='disabled'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl), "set dhcp.%s.ra='disabled'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl),
		 "del_list dhcp.%s.ra_flags='managed-config'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl),
		 "del_list dhcp.%s.ra_flags='other-config'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl), "add_list dhcp.%s.ra_flags='none'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	mesh_uci_batch_line("set dhcp.@dnsmasq[0].rebind_protection='0'");

	mesh_set_ula_prefix(ctx, "revert");
	mesh_log_debug(ctx, "peer handoff: network reload");
	service_cmd("network", "reload");
	mesh_log_debug(ctx, "peer handoff: network reload returned");
	service_cmd("dnsmasq", "restart");
	service_cmd("odhcpd", "restart");
	service_cmd("system", "restart");
	{
		char sysf[160];
		snprintf(sysf, sizeof(sysf), "/proc/sys/net/ipv6/conf/%s/accept_ra", ctx->device);
		f = fopen(sysf, "w");
		if (f) { fputs("2\n", f); fclose(f); }
	}
	mesh_get_mesh_iflist(ctx);
	ctx->failcount = 0;
	ctx->network_restart = 0;
}

static void portal_handoff(struct mesh_ctx *ctx)
{
	char setl[192];
	char path[128];
	FILE *f;

	mesh_uci_set("mesh11sd.mesh_params.mesh_connected_to_as", "1");
	snprintf(path, sizeof(path), "/var/run/udhcpc-%s.pid", ctx->device);
	if ((f = fopen(path, "r"))) {
		int pid = 0;
		if (fscanf(f, "%d", &pid) == 1 && pid > 0)
			kill(pid, SIGTERM);
		fclose(f);
	}

	mesh_log_debug(ctx, "This meshnode is an upstream portal");
	snprintf(setl, sizeof(setl), "set network.%s.proto='static'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	mesh_set_ula_prefix(ctx, "set");
	/* Portal IPv4 into UCI before netifd reload (v7 sets in autostartup;
	 * handoff must not leave a stale address for restore_ipv4 to stack). */
	if (ctx->portal_detect != 5)
		mesh_apply_portal_ipv4(ctx);
	service_cmd("network", "reload");

	if (!ctx->portal_ula[0])
		mesh_get_portal_ula(ctx, "get");

	mesh_uci_batch_line("set dhcp.@dnsmasq[0].authoritative='1'");
	snprintf(setl, sizeof(setl), "set dhcp.%s.ignore='0'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl), "set dhcp.%s.ra_default='2'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl), "set dhcp.%s.dhcpv6='server'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl), "set dhcp.%s.dhcpv4='server'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl), "set dhcp.%s.ra='server'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl), "del_list dhcp.%s.ra_flags='none'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl),
		 "add_list dhcp.%s.ra_flags='managed-config'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl),
		 "add_list dhcp.%s.ra_flags='other-config'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	mesh_uci_batch_line("set dhcp.@dnsmasq[0].rebind_protection='1'");

	/* v7 restore_ipv4: only if netifd did not put the address on yet */
	if (ctx->portal_detect != 4)
		mesh_restore_ipv4(ctx);

	service_cmd("dnsmasq", "restart");
	service_cmd("odhcpd", "restart");
	service_cmd("system", "restart");
	{
		char sysf[160];
		snprintf(sysf, sizeof(sysf), "/proc/sys/net/ipv6/conf/%s/accept_ra", ctx->device);
		f = fopen(sysf, "w");
		if (f) { fputs("0\n", f); fclose(f); }
	}
	if (ctx->daemon_startup)
		ctx->daemon_startup = 0;
}

int mesh_check_portal(struct mesh_ctx *ctx)
{
	mesh_log_debug(ctx, "Checking portal status");
	ctx->is_portal[0] = '\0';

	if (ctx->portal_detect == 2)
		return 0;

	mesh_get_portal_state(ctx);

	if (ctx->portal_detect == 3 || ctx->portal_detect == 5) {
		/* CPE/TPN special-cased earlier in v7; peer-like */
		return 0;
	}
	if (ctx->portal_detect == 4)
		return 0;

	if (strcmp(ctx->detected_state, "portal") != 0) {
		mesh_log_debug(ctx, "This IS NOT a layer 3 mesh portal");
		if (!strcmp(ctx->detected_state, "peer") && !strcmp(ctx->last_state, "initial"))
			snprintf(ctx->last_state, sizeof(ctx->last_state), "portal");
		if (!strcmp(ctx->detected_state, "peer") && !strcmp(ctx->last_state, "portal")) {
			snprintf(ctx->last_state, sizeof(ctx->last_state), "peer");
			peer_handoff(ctx);
		}
	} else {
		mesh_log_debug(ctx, "This IS a layer 3 mesh portal");
		if (!strcmp(ctx->detected_state, "portal") && !strcmp(ctx->last_state, "initial"))
			snprintf(ctx->last_state, sizeof(ctx->last_state), "peer");
		if (!strcmp(ctx->detected_state, "portal") && !strcmp(ctx->last_state, "peer")) {
			snprintf(ctx->last_state, sizeof(ctx->last_state), "portal");
			portal_handoff(ctx);
		}
	}
	mesh_portal_watchdog(ctx);
	write_is_portal(ctx);
	return 0;
}

int mesh_portal_watchdog(struct mesh_ctx *ctx)
{
	int auto_detect = (ctx->portal_detect == 1);

	if (strcmp(ctx->detected_state, "portal") != 0 && auto_detect) {
		if (ctx->portal_detect_threshold > ctx->failcount) {
			mesh_log_debug(ctx, "Portal Watchdog countdown [ %d ] ",
				       ctx->portal_detect_threshold - ctx->failcount);
			ctx->failcount++;
			return 0;
		}
		mesh_log_debug(ctx, "Portal Watchdog - checking status ....");
		mesh_get_portal_ula(ctx, "discover");
		mesh_log_debug(ctx, "Portal Watchdog probe link_to_portal [ %d ] portal_ula [ %s ]",
			       ctx->link_to_portal, ctx->portal_ula);
		if (ctx->link_to_portal == 0) {
			mesh_log_debug(ctx, "Portal Watchdog - portal detected ....");
			ctx->failcount = 0;
			ctx->network_restart = 0;
			snprintf(ctx->is_portal, sizeof(ctx->is_portal), "detected");
			snprintf(ctx->detected_state, sizeof(ctx->detected_state), "peer");
			write_is_portal(ctx);
			return 0;
		}
		if (ctx->network_restart == 0) {
			mesh_log_debug(ctx, "Link to portal fail, attempting to (re)establish, iteration [ %d ]",
				       ctx->failcount);
			ctx->failcount = 0;
			ctx->network_restart = 1;
			return 0;
		}
		if (ctx->network_restart == 1) {
			mesh_write_to_syslog(ctx, "err",
				"portal detect threshold reached, watchdog is monitoring");
			ctx->network_restart = 2;
			ctx->failcount = 0;
			return 0;
		}
		if (ctx->network_restart == 2) {
			mesh_write_to_syslog(ctx, "notice",
				"portal detect threshold exceeded, watchdog is restarting the network...");
			ctx->network_restart = 3;
			service_cmd("network", "restart");
			mesh_set_bridge_mac_addresses(ctx);
			ctx->failcount = 0;
			return 0;
		}
		if (ctx->network_restart == 3) {
			if (ctx->stop_on_error) {
				mesh_write_to_syslog(ctx, "err",
					"portal detect threshold exceeded... disabling mesh management");
				ctx->enabled = 0;
			} else if (ctx->reboot_on_error) {
				mesh_write_to_syslog(ctx, "err",
					"portal detect threshold exceeded... rebooting node");
				{
					char *argv[] = { "reboot", NULL };
					mesh_run_capture(argv, NULL, 0);
				}
			} else {
				ctx->failcount = 0;
				ctx->network_restart = 0;
			}
		}
	} else {
		ctx->failcount = 0;
		ctx->network_restart = 0;
	}
	return 0;
}

int mesh_get_portal_type(struct mesh_ctx *ctx)
{
	if (!strcmp(ctx->detected_state, "portal")) {
		snprintf(ctx->portal_type, sizeof(ctx->portal_type),
			 ctx->portal_detect == 4 ? "MBP" : "MRP");
		mesh_log_info(ctx, "Portal [ %s@%s ]", ctx->portal_type, ctx->devicemac);
		return 0;
	}
	mesh_get_portal_ula(ctx, "get");
	if (ctx->link_to_portal > 0) {
		snprintf(ctx->portal_type, sizeof(ctx->portal_type), "UNKNOWN");
		return 1;
	}
	snprintf(ctx->portal_type, sizeof(ctx->portal_type), "MRP");
	return 0;
}

int mesh_get_peers(struct mesh_ctx *ctx)
{
	int i;
	char out[4096];

	mesh_get_mesh_iflist(ctx);
	for (i = 0; i < ctx->iflist_count; i++) {
		char status[16];
		char *argv[] = { "iw", "dev", ctx->iflist[i], "mpath", "dump", NULL };
		mesh_wait_for_interface(ctx, ctx->iflist[i], status, sizeof(status));
		if (!strcmp(status, "down"))
			continue;
		mesh_run_capture(argv, out, sizeof(out));
	}
	return 0;
}

static int mesh_ndp_scan_peer(struct mesh_ctx *ctx)
{
	/* v7 ndp_scan_peer: ping ff02::2%br-<zone> to solicit routers for join */
	char br[64];
	char pingaddr[96];
	char *pargv[] = { "ping", "-c", "2", pingaddr, NULL };

	snprintf(br, sizeof(br), "br-%s", ctx->auto_mesh_network);
	snprintf(pingaddr, sizeof(pingaddr), "ff02::2%%%s", br);
	mesh_run_capture(pargv, NULL, 0);
	return 0;
}

int mesh_ccpy_setup(struct mesh_ctx *ctx)
{
	char show[4096];
	char *p, *line;
	char connectlist_path[128];
	FILE *cf;

	mesh_get_peers(ctx);
	mesh_ndp_scan_peer(ctx);

	snprintf(connectlist_path, sizeof(connectlist_path),
		 "%s/connectlist", MESH11SD_TMPDIR);
	cf = fopen(connectlist_path, "w");
	if (mesh_uci_show_filter("wireless", "mode='mesh'", show, sizeof(show)) != 0) {
		if (cf)
			fclose(cf);
		return 0;
	}
	p = show;
	while ((line = strsep(&p, " \n")) != NULL) {
		char sect[96], *eq, netkey[96], net[64], br[64];
		char out[4096];
		char *argv[] = { "ip", "-6", "neigh", NULL };
		char *np, *nline;

		eq = strchr(line, '=');
		if (!eq)
			continue;
		snprintf(sect, sizeof(sect), "%s", line);
		sect[eq - line] = '\0';
		snprintf(netkey, sizeof(netkey), "%s.network", sect);
		if (mesh_uci_get(netkey, net, sizeof(net)) != 0)
			continue;
		snprintf(br, sizeof(br), "network.%s.device", net);
		if (mesh_uci_get(br, br, sizeof(br)) != 0)
			snprintf(br, sizeof(br), "br-%s", net);
		if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
			np = out;
			while ((nline = strsep(&np, "\n")) != NULL) {
				char addr[96], iface[64], lladdr[8], mac[32];
				if (!strstr(nline, "fe80::") || !strstr(nline, "lladdr"))
					continue;
				if (sscanf(nline, "%95s dev %63s %7s %31s",
					   addr, iface, lladdr, mac) >= 4) {
					if (strcmp(iface, br) != 0 &&
					    strcmp(iface, net) != 0)
						continue;
					if (cf)
						fprintf(cf, "%s%%%s,%s\n", addr, iface, mac);
				}
			}
		}
		break; /* first mesh network only, as v7 */
	}
	if (cf)
		fclose(cf);
	return 0;
}

int mesh_check_odhcpd(struct mesh_ctx *ctx)
{
	/* v7 check_odhcpd: delay one checkinterval after portal so ULA is on
	 * lan before odhcpd restart (immediate restart races set_ula/netifd). */
	static int odhcpd_check;
	char lan_ra[32] = "";
	char vtun_ignore[32] = "";
	char key[96];
	int need_ra = 0;

	if (strcmp(ctx->detected_state, "portal") != 0) {
		odhcpd_check = 0;
		return 0;
	}
	snprintf(key, sizeof(key), "dhcp.%s.ra", ctx->auto_mesh_network);
	mesh_uci_get(key, lan_ra, sizeof(lan_ra));
	mesh_uci_get("dhcp.vtunlan.ignore", vtun_ignore, sizeof(vtun_ignore));
	if (!strcmp(lan_ra, "server"))
		need_ra = 1;
	if (!strcmp(vtun_ignore, "0"))
		need_ra = 1;
	if (!need_ra)
		return 0;
	if (odhcpd_check == 0) {
		odhcpd_check = 1;
	} else if (odhcpd_check == 1) {
		mesh_log_debug(ctx, "odhcpd restarting");
		service_cmd("odhcpd", "restart");
		odhcpd_check = 2;
	}
	return 0;
}
