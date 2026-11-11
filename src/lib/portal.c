/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 5: portal detect, peer/portal handoff, watchdog, ULA (v7-faithful).
 * Peer handoff sets network.<zone>.proto='dhcp' (not none), as v7.
 */
#include "mesh11sd.h"
#include <ctype.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

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

/* v7: dnsmasq restart must not block — with br-lan down it waits forever */
static void service_cmd_bg(const char *svc, const char *action)
{
	pid_t pid = fork();
	if (pid == 0) {
		char *argv[] = { "/sbin/service", (char *)svc, (char *)action, NULL };
		int dn = open("/dev/null", O_RDWR);
		if (dn >= 0) {
			dup2(dn, 0); dup2(dn, 1); dup2(dn, 2);
			if (dn > 2) close(dn);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
}

/* Split a portal ULA (fdxx:xxxx:xxxx:subnet::1 or the same with /64)
 * into the /48 base and the 4th hextet. Keeps the hextet text as-is so
 * leading zeros are not dropped. */
int mesh_ula_parts_from_addr(const char *addr, char *base, size_t baselen,
			     char *subnet, size_t subnetlen)
{
	char tmp[128];
	char *slash, *tp, *tok;
	char *h[8];
	int n = 0;

	if (!addr || !addr[0])
		return -1;
	snprintf(tmp, sizeof(tmp), "%s", addr);
	slash = strchr(tmp, '/');
	if (slash)
		*slash = '\0';
	tp = tmp;
	while (n < 8 && (tok = strsep(&tp, ":")) != NULL)
		h[n++] = tok;
	/* Need four leading hextets before any "::" compression hole. */
	if (n < 4 || !h[0][0] || !h[1][0] || !h[2][0] || !h[3][0])
		return -1;
	if (base && baselen)
		snprintf(base, baselen, "%s:%s:%s", h[0], h[1], h[2]);
	if (subnet && subnetlen)
		snprintf(subnet, subnetlen, "%s", h[3]);
	return 0;
}

static void apply_ula_from_portal_addr(struct mesh_ctx *ctx, const char *addr)
{
	char base[32], subnet[8];

	if (mesh_ula_parts_from_addr(addr, base, sizeof(base),
				     subnet, sizeof(subnet)) != 0)
		return;
	snprintf(ctx->ula_base, sizeof(ctx->ula_base), "%s", base);
	snprintf(ctx->ula_subnet, sizeof(ctx->ula_subnet), "%s", subnet);
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
		/* v7 also writes ip6hint on revert/set; it does not set ip6assign here. */
		if (ctx->ula_subnet[0]) {
			snprintf(setl, sizeof(setl), "set network.%s.ip6hint='%s'",
				 ctx->auto_mesh_network, ctx->ula_subnet);
			mesh_uci_batch_line(setl);
		}
		/* Only a portal should have netifd assign a local /64. Doing this
		 * on revert makes a peer keep preconf:hint::1 and can leave odhcpd
		 * advertising the old portal prefix. */
		if (!strcmp(mode, "set")) {
			snprintf(setl, sizeof(setl), "set network.%s.ip6assign='64'",
				 ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
		}
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
			p = strstr(line, "ula_subnet=\"");
			if (p)
				sscanf(p + 12, "%7[^\"]", ctx->ula_subnet);
			ctx->link_to_portal = ping_portal(ctx, ctx->portal_ula);
			if (ctx->link_to_portal == 0) {
				/* set_ula_prefix get just overwrote ula_subnet with
				 * this node's factory-hash hint. Use the selected
				 * portal's 4th hextet, as v7 does by sourcing
				 * active_portal after that get. */
				apply_ula_from_portal_addr(ctx, ctx->portal_ula);
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
		mesh_ensure_iface_arp_multicast(ctx, ctx->device);
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
	snprintf(setl, sizeof(setl), "set dhcp.%s.force='0'", ctx->auto_mesh_network);
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
	/* v7 disables RA; also drop ip6assign so netifd/odhcpd stop holding
	 * and advertising the old portal prefix after reload. */
	snprintf(setl, sizeof(setl), "del network.%s.ip6assign", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	/* Forget our own portal ULA so the next get discovers the real portal
	 * instead of pinging a leftover ::1 on this node. */
	unlink(MESH11SD_TMPDIR "/active_portal");
	ctx->portal_ula[0] = '\0';
	ctx->link_to_portal = 1;
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
	char vxname[32];
	char out[128];
	FILE *f;
	int vxlan_up = 0;
	int vxlan_pending = 1;

	mesh_uci_set("mesh11sd.mesh_params.mesh_connected_to_as", "1");
	snprintf(path, sizeof(path), "/var/run/udhcpc-%s.pid", ctx->device);
	if ((f = fopen(path, "r"))) {
		int pid = 0;
		if (fscanf(f, "%d", &pid) == 1 && pid > 0)
			kill(pid, SIGTERM);
		fclose(f);
	}

	mesh_log_debug(ctx, "This meshnode is an upstream portal");
	snprintf(vxname, sizeof(vxname), "vxlan%s",
		 ctx->tun_id[0] ? ctx->tun_id : "69");
	{
		char *show[] = { "ip", "link", "show", vxname, NULL };
		vxlan_up = (mesh_run_capture(show, out, sizeof(out)) == 0);
		vxlan_pending = !vxlan_up;
	}
	if (vxlan_up)
		mesh_uci_batch_line("set network.vtunlan.proto='static'");

	snprintf(setl, sizeof(setl), "set network.%s.proto='static'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	mesh_set_ula_prefix(ctx, "set");
	/* Portal IPv4 into UCI before netifd reload (v7 sets in autostartup;
	 * handoff must not leave a stale address for restore_ipv4 to stack). */
	if (ctx->portal_detect != 5)
		mesh_apply_portal_ipv4(ctx);
	/* v7: if vxlan is about to be created, check_vtunnel will network restart */
	if (!ctx->vtun_enable || !vxlan_pending)
		service_cmd("network", "reload");

	if (!ctx->portal_ula[0])
		mesh_get_portal_ula(ctx, "get");

	mesh_uci_batch_line("set dhcp.@dnsmasq[0].authoritative='1'");
	mesh_uci_batch_line("set dhcp.@dnsmasq[0].logfacility='-'");
	mesh_uci_batch_line("set dhcp.@dnsmasq[0].quietdhcp='1'");
	{
		char cfgip[64] = "";
		char key[96];
		snprintf(key, sizeof(key), "network.%s.ipaddr", ctx->auto_mesh_network);
		if (mesh_uci_get(key, cfgip, sizeof(cfgip)) == 0 && cfgip[0]) {
			snprintf(setl, sizeof(setl),
				 "del_list dhcp.@dnsmasq[0].server='%s'", cfgip);
			mesh_uci_batch_line(setl);
		}
	}
	snprintf(setl, sizeof(setl), "set dhcp.%s.ignore='0'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	/* OpenWrt dnsmasq init runs udhcpc dhcp_check on the bridge; another
	 * portal's DHCP offer makes it refuse the lan range unless force=1.
	 * v7 sets authoritative=1 (multi-server NAK) but not force; without force
	 * a dnsmasq restart while a second portal is visible drops lan DHCP.
	 * ARP pin (mesh_ARP) only drops foreign gateway ARP — not UDP 67/68. */
	snprintf(setl, sizeof(setl), "set dhcp.%s.force='1'", ctx->auto_mesh_network);
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
		 "del_list dhcp.%s.ra_flags='managed-config'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl),
		 "del_list dhcp.%s.ra_flags='other-config'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl),
		 "add_list dhcp.%s.ra_flags='managed-config'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	snprintf(setl, sizeof(setl),
		 "add_list dhcp.%s.ra_flags='other-config'", ctx->auto_mesh_network);
	mesh_uci_batch_line(setl);
	mesh_uci_batch_line("set dhcp.@dnsmasq[0].rebind_protection='1'");

	/* v7: enable vtunlan DHCP when vxlan is already up */
	if (vxlan_up) {
		mesh_uci_batch_line("set dhcp.vtunlan.dhcpv4='server'");
		mesh_uci_batch_line("set dhcp.vtunlan.dhcpv6='server'");
		mesh_uci_batch_line("set dhcp.vtunlan.ra='server'");
		mesh_uci_batch_line("set dhcp.vtunlan.ignore='0'");
		mesh_uci_batch_line("set dhcp.vtunlan.force='1'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='none'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='managed-config'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='other-config'");
		mesh_uci_batch_line("add_list dhcp.vtunlan.ra_flags='managed-config'");
		mesh_uci_batch_line("add_list dhcp.vtunlan.ra_flags='other-config'");
		mesh_uci_batch_line("set dhcp.@dnsmasq[0].rebind_protection='1'");
	}

	/* v7 restore_ipv4: only if netifd did not put the address on yet */
	if (ctx->portal_detect != 4)
		mesh_restore_ipv4(ctx);

	/* Non-blocking — v7 create_vxlan warns blocking restart hangs on br-lan down */
	mesh_log_debug(ctx, "dnsmasq restarting");
	service_cmd_bg("dnsmasq", "restart");
	service_cmd_bg("odhcpd", "restart");
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



/* Re-apply the role overlay after the uncommitted changes were dropped
 * (managed.c). */
void mesh_peer_reapply(struct mesh_ctx *ctx)
{
	snprintf(ctx->last_state, sizeof(ctx->last_state), "peer");
	peer_handoff(ctx);
}

void mesh_portal_reapply(struct mesh_ctx *ctx)
{
	snprintf(ctx->last_state, sizeof(ctx->last_state), "portal");
	portal_handoff(ctx);
}

/* Ensure portal dhcp sections have force=1 so OpenWrt dnsmasq init does not
 * drop ranges when another portal answers the udhcpc dhcp_check probe.
 * Idempotent; restarts dnsmasq once if UCI changed (heals pre-alpha20 portals). */
int mesh_ensure_portal_dhcp_force(struct mesh_ctx *ctx)
{
	char cur[16];
	char key[96];
	int changed = 0;

	if (strcmp(ctx->detected_state, "portal") != 0)
		return 0;

	snprintf(key, sizeof(key), "dhcp.%s.force", ctx->auto_mesh_network);
	if (mesh_uci_get(key, cur, sizeof(cur)) != 0 || atoi(cur) != 1) {
		mesh_uci_set(key, "1");
		changed = 1;
	}
	if (ctx->vtun_enable) {
		char vxname[32];
		char out[64];
		char *show[] = { "ip", "link", "show", vxname, NULL };
		int vxlan_up;

		snprintf(vxname, sizeof(vxname), "vxlan%s",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		show[3] = vxname;
		vxlan_up = (mesh_run_capture(show, out, sizeof(out)) == 0);
		if (vxlan_up) {
			if (mesh_uci_get("dhcp.vtunlan.force", cur, sizeof(cur)) != 0 ||
			    atoi(cur) != 1) {
				mesh_uci_set("dhcp.vtunlan.force", "1");
				changed = 1;
			}
		}
	}
	if (changed) {
		mesh_log_notice(ctx,
				"dhcp force=1 on portal (defeat dnsmasq dhcp_check vs other portals)");
		service_cmd_bg("dnsmasq", "restart");
	}
	return changed;
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
		char sect[96], *eq, netkey[96], net[64], devkey[96], br[64];
		char out[4096];
		char *argv[] = { "ip", "-6", "neigh", NULL };
		char *np, *nline;

		/* line is "wireless.<section>.mode='mesh'"; keep "wireless.<section>" */
		eq = strstr(line, ".mode=");
		if (!eq)
			continue;
		snprintf(sect, sizeof(sect), "%s", line);
		sect[eq - line] = '\0';
		snprintf(netkey, sizeof(netkey), "%s.network", sect);
		if (mesh_uci_get(netkey, net, sizeof(net)) != 0)
			continue;
		/* The key needs its own buffer: mesh_uci_get clears the output first. */
		snprintf(devkey, sizeof(devkey), "network.%s.device", net);
		if (mesh_uci_get(devkey, br, sizeof(br)) != 0 || !br[0])
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

/* v7 manage_fallback_ula_route: ensure mesh11sd-owned ULAs on bridges.
 * Each bridge gets:
 *   1) prefix:ip6hint::1  (portal gateway ULA; portal add / peer delete)
 *   2) prefix:ip6hint:EUI64 from bridge MAC (deterministic; portal_detect!=4)
 * Do NOT touch kernel_ra / RA-derived addresses — only mesh11sd-owned ::1
 * and EUI-64 IIDs (and stale ones with wrong subnet).
 */
static int ipv6_addr_present(const char *dev, const char *addr_with_pfx)
{
	char out[4096];
	char *argv[] = { "ip", "-6", "addr", "show", "dev", (char *)dev, NULL };

	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return 0;
	return strstr(out, addr_with_pfx) != NULL;
}

static int ipv6_addr_add(const char *dev, const char *addr_with_pfx)
{
	char *argv[] = {
		"ip", "-6", "addr", "add", (char *)addr_with_pfx, "dev",
		(char *)dev, NULL
	};
	return mesh_run_capture(argv, NULL, 0);
}

static int ipv6_addr_del(const char *dev, const char *addr_with_pfx)
{
	char *argv[] = {
		"ip", "-6", "addr", "del", (char *)addr_with_pfx, "dev",
		(char *)dev, NULL
	};
	return mesh_run_capture(argv, NULL, 0);
}

/* v7: no U/L bit flip — insert fffe into bare MAC */
static int eui64_from_mac(const char *mac, char *out, size_t outlen)
{
	char bare[32];
	char eui[20];
	size_t i, j = 0;

	if (!mac || !mac[0])
		return -1;
	for (i = 0; mac[i] && j < sizeof(bare) - 1; i++) {
		if (mac[i] == ':' || mac[i] == '-')
			continue;
		bare[j++] = (char)tolower((unsigned char)mac[i]);
	}
	bare[j] = '\0';
	if (j != 12)
		return -1;
	/* v7: no U/L flip — insert fffe into bare MAC */
	snprintf(eui, sizeof(eui), "%.6sfffe%.6s", bare, bare + 6);
	snprintf(out, outlen, "%.4s:%.4s:%.4s:%.4s",
		 eui, eui + 4, eui + 8, eui + 12);
	return 0;
}

static int bridge_mac(const char *dev, char *out, size_t outlen)
{
	char path[128];
	FILE *f;
	char line[64];
	size_t n;

	snprintf(path, sizeof(path), "/sys/class/net/%s/address", dev);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	n = strcspn(line, "\r\n");
	line[n] = '\0';
	if (n < 11)
		return -1;
	snprintf(out, outlen, "%s", line);
	return 0;
}

/* Ensure ::1 (Part 1) and EUI-64 (Part 2) ULAs for one bridge.
 * portal_ula_1 = "prefix:hint::1" WITHOUT /64 (v7 $portal_ula); may be NULL
 * to skip Part 1. subnet = this bridge's ip6hint for the EUI-64 /64. */
static void ensure_bridge_ulas(struct mesh_ctx *ctx, const char *dev,
			       const char *portal_ula_1, const char *subnet,
			       int is_portal)
{
	char portal_addr[96];
	char eui[40];
	char mac[32];
	char ula_addr[128];

	if (!dev || !dev[0] || !ctx->ula_base[0])
		return;

	/* Part 1: portal ::1 — add on portal, delete on peer if leftover */
	if (portal_ula_1 && portal_ula_1[0]) {
		snprintf(portal_addr, sizeof(portal_addr), "%s/64", portal_ula_1);
		if (is_portal) {
			mesh_log_debug(ctx,
				       "manage_fallback_ula_route - Node is a portal");
			mesh_log_debug(ctx,
				       "portal_ula_addr is [ %s ], checking...",
				       portal_addr);
			if (!ipv6_addr_present(dev, portal_addr)) {
				mesh_log_info(ctx,
					      "Portal mode → adding portal ULA %s to bridge %s",
					      portal_addr, dev);
				if (ipv6_addr_add(dev, portal_addr) == 0)
					mesh_log_notice(ctx,
							"Portal ULA %s added to %s",
							portal_addr, dev);
				else
					mesh_log_err(ctx,
						     "Failed to add portal ULA %s to %s",
						     portal_addr, dev);
			} else {
				mesh_log_debug(ctx,
					       "portal_ula_addr [ %s ] is present",
					       portal_addr);
			}
		} else {
			mesh_log_debug(ctx,
				       "manage_fallback_ula_route - Node is a peer");
			mesh_log_debug(ctx,
				       "portal_ula_addr is [ %s ], checking...",
				       portal_addr);
			if (ipv6_addr_present(dev, portal_addr)) {
				mesh_log_info(ctx,
					      "Peer mode → deleting portal ULA %s from bridge %s",
					      portal_addr, dev);
				if (ipv6_addr_del(dev, portal_addr) == 0)
					mesh_log_notice(ctx,
							"Portal ULA %s deleted from %s",
							portal_addr, dev);
				else
					mesh_log_err(ctx,
						     "Failed to remove portal ULA %s from %s",
						     portal_addr, dev);
			}
		}
	}

	/* Part 2: deterministic EUI-64 ULA (all nodes when portal_detect != 4) */
	if (ctx->portal_detect == 4 || !subnet || !subnet[0])
		return;
	if (bridge_mac(dev, mac, sizeof(mac)) != 0)
		return;
	if (eui64_from_mac(mac, eui, sizeof(eui)) != 0)
		return;
	snprintf(ula_addr, sizeof(ula_addr), "%s:%s:%s/64",
		 ctx->ula_base, subnet, eui);
	if (!ipv6_addr_present(dev, ula_addr)) {
		mesh_log_info(ctx,
			      "Adding deterministic ULA address %s to bridge %s",
			      ula_addr, dev);
		if (ipv6_addr_add(dev, ula_addr) == 0)
			mesh_log_notice(ctx, "ULA address %s added to %s",
					ula_addr, dev);
		else
			mesh_log_err(ctx, "Failed to add ULA address %s to %s",
				     ula_addr, dev);
	}
}

/* Compare IPv6 hextets numerically so "0abc" and "abc" match. */
static int hextet_eq(const char *a, const char *b)
{
	char *ea, *eb;
	unsigned long ia, ib;

	if (!a || !b || !a[0] || !b[0])
		return 0;
	ia = strtoul(a, &ea, 16);
	ib = strtoul(b, &eb, 16);
	if (ea == a || eb == b)
		return 0;
	return ia == ib;
}

static int addr_is_gateway_ula(const char *addr)
{
	size_t n;

	if (!addr)
		return 0;
	n = strlen(addr);
	return n >= 3 && strcmp(addr + n - 3, "::1") == 0;
}

/* Remove stale mesh11sd-owned ULAs (wrong-subnet ::1 / EUI64) relative to
 * the current portal. Leaves kernel_ra alone. On a peer, also drop any
 * leftover global ::1 (netifd ip6assign on a reverted ula_prefix). An empty
 * want_subnet means every mesh-owned address on ula_base is stale. */
static void cleanup_stale_bridge_ulas(struct mesh_ctx *ctx, const char *dev,
				     const char *want_subnet, int is_portal)
{
	char out[8192];
	char mac[32];
	char eui[40] = "";
	char *argv[] = { "ip", "-6", "addr", "show", "dev", (char *)dev, NULL };
	char *p, *line;
	char prefix_match[64];
	int have_eui;

	if (!dev[0] || !ctx->ula_base[0])
		return;
	have_eui = (bridge_mac(dev, mac, sizeof(mac)) == 0 &&
		    eui64_from_mac(mac, eui, sizeof(eui)) == 0);
	snprintf(prefix_match, sizeof(prefix_match), "inet6 %s:", ctx->ula_base);

	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return;
	p = out;
	while ((line = strsep(&p, "\n")) != NULL) {
		char addr[128];
		char addr_only[128];
		char old_subnet[16] = "";
		char *slash;
		char *tok;
		int is_gw, on_mesh, keep, owned;

		tok = strstr(line, "inet6 ");
		if (!tok)
			continue;
		tok += 6;
		sscanf(tok, "%127s", addr);
		snprintf(addr_only, sizeof(addr_only), "%s", addr);
		slash = strchr(addr_only, '/');
		if (slash)
			*slash = '\0';
		is_gw = addr_is_gateway_ula(addr_only);
		on_mesh = strstr(line, prefix_match) != NULL;

		owned = 0;
		keep = 0;
		if (on_mesh &&
		    mesh_ula_parts_from_addr(addr_only, NULL, 0,
					     old_subnet, sizeof(old_subnet)) == 0) {
			keep = (want_subnet && want_subnet[0] &&
				hextet_eq(old_subnet, want_subnet));
			/* mesh11sd-owned: our EUI-64 or a portal ::1 */
			if (is_gw || (have_eui && eui[0] && strstr(addr_only, eui)))
				owned = 1;
		}
		if (owned && !keep) {
			if (ipv6_addr_del(dev, addr) == 0)
				mesh_log_notice(ctx,
						"Removed stale portal address %s from %s",
						addr, dev);
			continue;
		}

		/* Peer leftover: netifd ip6assign ::1 on a different ula_base
		 * (the reverted OpenWrt prefix). Do not touch link-local. */
		if (!is_portal && !on_mesh && is_gw &&
		    strstr(line, "scope global")) {
			if (ipv6_addr_del(dev, addr) == 0)
				mesh_log_notice(ctx,
						"Removed stale portal address %s from %s",
						addr, dev);
		}
	}
}

static void cleanup_stale_onlink_routes(struct mesh_ctx *ctx, const char *dev,
					const char *want_subnet)
{
	char out[8192];
	char *argv[] = {
		"ip", "-6", "route", "show", "dev", (char *)dev,
		"proto", "static", NULL
	};
	char *p, *line;
	char pfx_head[64];

	if (!dev || !dev[0] || !ctx->ula_base[0])
		return;
	snprintf(pfx_head, sizeof(pfx_head), "%s:", ctx->ula_base);
	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return;
	p = out;
	while ((line = strsep(&p, "\n")) != NULL) {
		char prefix[96];
		char old_subnet[16];
		char *delv[] = {
			"ip", "-6", "route", "del", prefix, "dev", (char *)dev,
			"onlink", "metric", "512", "proto", "static", NULL
		};

		if (!strstr(line, "metric 512"))
			continue;
		if (sscanf(line, "%95s", prefix) != 1)
			continue;
		if (strncmp(prefix, pfx_head, strlen(pfx_head)) != 0)
			continue;
		if (!strstr(prefix, "::/64"))
			continue;
		if (mesh_ula_parts_from_addr(prefix, NULL, 0,
					     old_subnet, sizeof(old_subnet)) != 0)
			continue;
		if (want_subnet && want_subnet[0] &&
		    hextet_eq(old_subnet, want_subnet))
			continue;
		if (mesh_run_capture(delv, NULL, 0) == 0)
			mesh_log_notice(ctx, "Removed stale portal route %s from %s",
					prefix, dev);
	}
}

static void cleanup_stale_portal_neighs(struct mesh_ctx *ctx, const char *dev,
					const char *want_subnet)
{
	char out[8192];
	char *argv[] = {
		"ip", "-6", "neigh", "show", "dev", (char *)dev, NULL
	};
	char *p, *line;
	char pfx_head[64];

	if (!dev || !dev[0] || !ctx->ula_base[0])
		return;
	snprintf(pfx_head, sizeof(pfx_head), "%s:", ctx->ula_base);
	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return;
	p = out;
	while ((line = strsep(&p, "\n")) != NULL) {
		char neigh[96];
		char old_subnet[16];
		char *delv[] = {
			"ip", "-6", "neigh", "del", neigh, "dev",
			(char *)dev, NULL
		};

		if (sscanf(line, "%95s", neigh) != 1)
			continue;
		if (!addr_is_gateway_ula(neigh))
			continue;
		if (strncmp(neigh, pfx_head, strlen(pfx_head)) != 0)
			continue;
		if (mesh_ula_parts_from_addr(neigh, NULL, 0,
					     old_subnet, sizeof(old_subnet)) != 0)
			continue;
		if (want_subnet && want_subnet[0] &&
		    hextet_eq(old_subnet, want_subnet))
			continue;
		if (mesh_run_capture(delv, NULL, 0) == 0)
			mesh_log_notice(ctx,
					"Removed stale portal neighbour %s from %s",
					neigh, dev);
	}
}

static void ensure_sticky_onlink_route(struct mesh_ctx *ctx, const char *dev,
				      const char *subnet)
{
	char pfx[96];
	char out[4096];
	char *show[] = { "ip", "-6", "route", "show", pfx, NULL };
	char *add[] = {
		"ip", "-6", "route", "add", pfx, "dev", (char *)dev,
		"onlink", "metric", "512", "proto", "static", NULL
	};

	if (!dev[0] || !ctx->ula_base[0] || !subnet[0])
		return;
	snprintf(pfx, sizeof(pfx), "%s:%s::/64", ctx->ula_base, subnet);
	show[4] = pfx;
	add[4] = pfx;
	if (mesh_run_capture(show, out, sizeof(out)) == 0 &&
	    strstr(out, "proto static") && strstr(out, "metric 512") &&
	    strstr(out, "onlink"))
		return;
	mesh_log_info(ctx,
		      "Adding sticky fallback on-link route %s dev %s (peer auto-detect mode)",
		      pfx, dev);
	if (mesh_run_capture(add, NULL, 0) == 0)
		mesh_log_notice(ctx, "Sticky fallback on-link route added on %s",
				dev);
	else
		mesh_log_err(ctx, "Failed to add fallback route on %s", dev);
}

int mesh_manage_fallback_ula_route(struct mesh_ctx *ctx)
{
	const char *lan_dev;
	char tun_dev[32];
	int is_portal;
	int got_portal;

	mesh_refresh_bridgemac(ctx);
	lan_dev = ctx->device[0] ? ctx->device : "";
	if (!lan_dev[0]) {
		mesh_log_warn(ctx,
			      "No %s/devicemac found → cannot determine bridge for ULA/route",
			      MESH11SD_TMPDIR);
		return 1;
	}

	mesh_set_ula_prefix(ctx, "get");
	if (!ctx->ula_prefix[0] || !ctx->ula_base[0]) {
		mesh_log_warn(ctx, "Could not determine ULA prefix for address/route");
		return 1;
	}
	mesh_log_debug(ctx, "ula_base [ %s ]", ctx->ula_base);

	got_portal = (mesh_get_portal_ula(ctx, "get") == 0);
	is_portal = (strcmp(ctx->detected_state, "portal") == 0);

	/* A peer's ULA subnet is only the current portal's 4th hextet, never
	 * the local factory-hash hint left by set_ula_prefix get. If no portal
	 * is known yet, do not invent one — just drop leftovers. */
	{
		const char *lan_gw = ctx->portal_ula[0] ? ctx->portal_ula : NULL;
		const char *lan_sub = "";
		const char *rdev = lan_dev;

		if (is_portal)
			lan_sub = ctx->ula_subnet;
		else if (got_portal && ctx->ula_subnet[0])
			lan_sub = ctx->ula_subnet;

		ensure_bridge_ulas(ctx, lan_dev, lan_gw, lan_sub, is_portal);
		cleanup_stale_bridge_ulas(ctx, lan_dev, lan_sub, is_portal);
		cleanup_stale_onlink_routes(ctx, lan_dev, lan_sub);
		cleanup_stale_portal_neighs(ctx, lan_dev, lan_sub);

		if (ctx->portal_detect == 3)
			rdev = "br-wan";
		if (lan_sub[0])
			ensure_sticky_onlink_route(ctx, rdev, lan_sub);
	}

	/* br-tun: same two ULAs using vtunlan ip6hint (ula_vxsubnet) */
	if (ctx->vtun_enable && ctx->tun_id[0] && ctx->ula_vxsubnet[0]) {
		char syspath[64];
		char vtun_gw[96];
		snprintf(tun_dev, sizeof(tun_dev), "br-tun%s", ctx->tun_id);
		snprintf(syspath, sizeof(syspath), "/sys/class/net/%s", tun_dev);
		snprintf(vtun_gw, sizeof(vtun_gw), "%s:%s::1",
			 ctx->ula_base, ctx->ula_vxsubnet);
		if (access(syspath, F_OK) == 0) {
			ensure_bridge_ulas(ctx, tun_dev, vtun_gw, ctx->ula_vxsubnet,
					   is_portal);
			cleanup_stale_bridge_ulas(ctx, tun_dev, ctx->ula_vxsubnet,
						  is_portal);
			cleanup_stale_onlink_routes(ctx, tun_dev, ctx->ula_vxsubnet);
			cleanup_stale_portal_neighs(ctx, tun_dev, ctx->ula_vxsubnet);
		}
	}

	return got_portal || is_portal ? 0 : 1;
}
