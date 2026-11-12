/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CPE mode (portal_detect 3), ported from v7.
 *
 * A CPE is a mesh peer that treats the mesh backhaul as its wan. The mesh
 * vif joins br-wan, which gets its IPv4 by DHCP from the backhaul. The
 * premises lan is a private routed subnet (IPv4 NAT plus NAT66, or prefix
 * delegation) served on lan ports and the MeshGate APs. The VTunnel APs
 * are bridged over the vxlan to the portal, so br-tun has no address.
 *
 * Everything here is staged (uncommitted) UCI; nothing is written to flash.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mesh11sd.h"

#define CPE_BRIDGE "br-wan"

static void stage(const char *fmt, const char *arg)
{
	char line[256];

	snprintf(line, sizeof(line), fmt, arg);
	mesh_uci_batch_line(line);
}

/* Find the firewall zone named wan, as v7 does (zones 0 to 7). */
static int find_wan_zone(char *sect, size_t len)
{
	char key[64], name[64];
	int i;

	for (i = 0; i < 8; i++) {
		snprintf(key, sizeof(key), "firewall.@zone[%d].name", i);
		if (mesh_uci_get(key, name, sizeof(name)) != 0)
			continue;
		name[strcspn(name, "\r\n")] = '\0';
		if (!strcmp(name, "wan")) {
			snprintf(sect, len, "firewall.@zone[%d]", i);
			return 0;
		}
	}
	return -1;
}

/* Delete a dhcp option if it is set to relay. */
static void drop_relay(const char *key)
{
	char val[32] = "";

	if (mesh_uci_get(key, val, sizeof(val)) == 0 && !strncmp(val, "relay", 5))
		stage("delete %s", key);
}

/*
 * v7 autostartup CPE block (v7 ~10878 and ~10950-11102). Called after the
 * lan IPv4 has been staged (mesh_apply_portal_ipv4), before the startup
 * network restart.
 */
int mesh_cpe_stage_network(struct mesh_ctx *ctx)
{
	char wanif[64] = "";
	char zone[64];
	char hostname[32] = "meshnode";
	char setl[192];
	int pd;

	if (ctx->portal_detect != 3 || ctx->commit_all)
		return 0;
	if (mesh_uci_get("network.wan", setl, sizeof(setl)) != 0 || !setl[0]) {
		mesh_log_err(ctx, "CPE mode needs a wan interface, none found");
		return -1;
	}
	pd = !strcmp(ctx->cpe_mode, "pd");

	if (!ctx->factory_mac[0])
		mesh_get_factory_mac(ctx, ctx->factory_mac, sizeof(ctx->factory_mac));
	if (strlen(ctx->factory_mac) >= 17)
		snprintf(hostname, sizeof(hostname), "meshnode-%c%c%c%c",
			 ctx->factory_mac[12], ctx->factory_mac[13],
			 ctx->factory_mac[15], ctx->factory_mac[16]);

	/* lan: the premises subnet, a static router address and a server */
	mesh_uci_batch_line("set network.lan.proto='static'");
	mesh_uci_batch_line("set network.lan.ip6assign='64'");
	mesh_uci_batch_line("set dhcp.lan.ignore='0'");
	mesh_uci_batch_line("set dhcp.lan.dhcpv4='server'");
	mesh_uci_batch_line("set dhcp.lan.dhcpv6='server'");
	mesh_uci_batch_line("set dhcp.lan.ra='server'");
	mesh_uci_batch_line("set dhcp.lan.ra_slaac='1'");
	mesh_uci_batch_line("set dhcp.lan.ra_default='2'");
	mesh_uci_batch_line("set dhcp.lan.ra_lifetime='3600'");
	mesh_uci_batch_line("set dhcp.lan.ra_mininterval='20'");
	mesh_uci_batch_line("set dhcp.lan.ra_maxinterval='60'");
	/* No relay: clear what a v7 relay setup may have left on flash */
	mesh_uci_batch_line("delete dhcp.lan.ndp");
	if (mesh_uci_get("dhcp.wan6", setl, sizeof(setl)) == 0 && setl[0]) {
		mesh_uci_batch_line("delete dhcp.wan6.master");
		drop_relay("dhcp.wan6.ra");
		drop_relay("dhcp.wan6.dhcpv6");
		drop_relay("dhcp.wan6.ndp");
	}

	/* br-wan: the mesh vif (network wan) and the wan port */
	mesh_uci_get("network.wan.device", wanif, sizeof(wanif));
	mesh_uci_batch_line("set network.cpewan=device");
	mesh_uci_batch_line("set network.cpewan.name='" CPE_BRIDGE "'");
	mesh_uci_batch_line("set network.cpewan.type='bridge'");
	if (wanif[0] && strcmp(wanif, CPE_BRIDGE) != 0) {
		stage("del_list network.cpewan.ports='%s'", wanif);
		stage("add_list network.cpewan.ports='%s'", wanif);
	}

	/* wan: IPv4 by DHCP from the mesh backhaul */
	mesh_uci_batch_line("set network.wan.device='" CPE_BRIDGE "'");
	mesh_uci_batch_line("set network.wan.proto='dhcp'");
	mesh_uci_batch_line("set network.wan.defaultroute='1'");
	mesh_uci_batch_line("set network.wan.peerdns='1'");
	mesh_uci_batch_line("set network.wan.disabled='0'");
	stage("set network.wan.hostname='%s'", hostname);

	/* wan6: an address from the backhaul; a prefix only in pd mode */
	mesh_uci_batch_line("set network.wan6=interface");
	mesh_uci_batch_line("set network.wan6.device='" CPE_BRIDGE "'");
	mesh_uci_batch_line("set network.wan6.proto='dhcpv6'");
	mesh_uci_batch_line("set network.wan6.reqaddress='try'");
	mesh_uci_batch_line(pd ? "set network.wan6.reqprefix='64'" :
				 "set network.wan6.reqprefix='no'");
	mesh_uci_batch_line(pd ? "set network.wan6.delegate='1'" :
				 "set network.wan6.delegate='0'");
	stage("set network.wan6.hostname='%s'", hostname);

	/* no DHCP or RA from this node towards the mesh */
	mesh_uci_batch_line("set dhcp.wan=dhcp");
	mesh_uci_batch_line("set dhcp.wan.interface='wan'");
	mesh_uci_batch_line("set dhcp.wan.ignore='1'");
	mesh_uci_batch_line("set dhcp.wan.dhcpv4='disabled'");
	mesh_uci_batch_line("set dhcp.wan.dhcpv6='disabled'");
	mesh_uci_batch_line("set dhcp.wan.ra='disabled'");
	mesh_uci_batch_line("delete dhcp.wan.ra_dns");
	mesh_uci_batch_line("delete dhcp.wan.ra_flags");
	mesh_uci_batch_line("add_list dhcp.wan.ra_flags='none'");

	mesh_uci_batch_line("set dhcp.@dnsmasq[0].rebind_protection='1'");
	mesh_uci_batch_line("set dhcp.@dnsmasq[0].logfacility='-'");
	mesh_uci_batch_line("set dhcp.@dnsmasq[0].quietdhcp='1'");

	/* wan zone: NAT, plus NAT66 unless prefixes are delegated */
	if (find_wan_zone(zone, sizeof(zone)) == 0) {
		stage("set %s.input='ACCEPT'", zone);
		stage("set %s.masq='1'", zone);
		stage("set %s.mtu_fix='1'", zone);
		snprintf(setl, sizeof(setl), "set %s.masq6='%d'", zone, pd ? 0 : 1);
		mesh_uci_batch_line(setl);
	} else {
		mesh_log_err(ctx, "Unable to find wan zone - CPE NAT is not configured");
	}

	mesh_log_notice(ctx, "CPE mode [ %s ]: lan is the routed subnet, mesh is wan on %s",
			ctx->cpe_mode, CPE_BRIDGE);
	return 0;
}

/*
 * v7 cpe_conf one-shot (v7 ~11390-11452), first pass of the main loop.
 * The startup restart has already applied network, dnsmasq and firewall.
 * odhcpd was stopped at startup and only a portal restarts it, so start
 * it here for the lan RA and DHCPv6.
 */
int mesh_cpe_conf(struct mesh_ctx *ctx)
{
	static int cpe_conf;
	char *odhcpd[] = { "/sbin/service", "odhcpd", "restart", NULL };

	if (ctx->portal_detect != 3 || cpe_conf)
		return 0;
	cpe_conf = 1;

	/* To other mesh nodes this looks like a peer */
	if (!ctx->commit_all)
		mesh_uci_set("mesh11sd.mesh_params.mesh_connected_to_as", "0");
	mesh_log_notice(ctx, "CPE mode");
	mesh_run_capture(odhcpd, NULL, 0);
	return 0;
}

/*
 * v7 ensure_cpe_multicast_discovery_rule (v7 ~8594-8636): let the portal
 * find this node by multicast ping to ff02::2 on wan. Runtime nft only;
 * a firewall restart drops these and the next pass adds them again.
 */
int mesh_cpe_discovery_rule(struct mesh_ctx *ctx)
{
	char out[8192];
	char *list_in[] = { "nft", "list", "chain", "inet", "fw4", "input_wan", NULL };
	char *list_out[] = { "nft", "list", "chain", "inet", "fw4", "accept_to_wan", NULL };
	char *add_in[] = {
		"nft", "insert", "rule", "inet", "fw4", "input_wan",
		"meta", "l4proto", "ipv6-icmp", "ip6", "daddr", "ff02::2",
		"counter", "accept",
		"comment", "\"mesh11sd: Allow multicast discovery from portal\"", NULL
	};
	char *add_out[] = {
		"nft", "insert", "rule", "inet", "fw4", "accept_to_wan",
		"meta", "nfproto", "ipv6", "meta", "l4proto", "ipv6-icmp", "accept",
		"comment", "\"mesh11sd: Allow ICMPv6 output on WAN (CPE discovery)\"", NULL
	};
	int added = 0;

	if (ctx->portal_detect != 3)
		return 0;

	if (mesh_run_capture(list_in, out, sizeof(out)) == 0 &&
	    !strstr(out, "ip6 daddr ff02::2")) {
		if (mesh_run_capture(add_in, NULL, 0) == 0) {
			mesh_log_debug(ctx, "CPE mode: Added incoming ff02::2 rule to input_wan");
			added = 1;
		}
	}
	if (mesh_run_capture(list_out, out, sizeof(out)) == 0 &&
	    !strstr(out, "CPE discovery")) {
		if (mesh_run_capture(add_out, NULL, 0) == 0) {
			mesh_log_debug(ctx, "CPE mode: Added outgoing ICMPv6 rule to accept_to_wan");
			added = 1;
		}
	}
	if (added)
		mesh_log_notice(ctx, "CPE mode: Added nft rules for multicast discovery from portal");
	return 0;
}
