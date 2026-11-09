/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 6: bridge MAC walk, STP priority, vxlan bring-up, bridge-loop nft.
 */
#include "mesh11sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int mesh_convert_to_la(const char *mac, char *out, size_t outlen)
{
	unsigned o[6];

	if (!mac || sscanf(mac, "%x:%x:%x:%x:%x:%x",
			   &o[0], &o[1], &o[2], &o[3], &o[4], &o[5]) != 6)
		return -1;
	o[0] |= 0x02; /* locally administered */
	snprintf(out, outlen, "%02x:%02x:%02x:%02x:%02x:%02x",
		 o[0], o[1], o[2], o[3], o[4], o[5]);
	return 0;
}

int mesh_get_indexed_mac(struct mesh_ctx *ctx, const char *devname,
			 char *out, size_t outlen)
{
	char last[64] = "";
	char maclist_path[128];
	char last_path[128];
	char line[128];
	FILE *f;
	unsigned o[6];
	char base[32];

	snprintf(maclist_path, sizeof(maclist_path), "%s/maclist", MESH11SD_TMPDIR);
	snprintf(last_path, sizeof(last_path), "%s/last_mac_indexed", MESH11SD_TMPDIR);

	/* already assigned? */
	f = fopen(maclist_path, "r");
	if (f) {
		char key[96];
		snprintf(key, sizeof(key), "%s_mac=", devname);
		while (fgets(line, sizeof(line), f)) {
			if (!strncmp(line, key, strlen(key))) {
				sscanf(line + strlen(key), "%31s", out);
				fclose(f);
				return 127; /* already assigned, as v7 */
			}
		}
		fclose(f);
	}

	if (access(last_path, R_OK) == 0) {
		f = fopen(last_path, "r");
		if (f) {
			if (fgets(line, sizeof(line), f)) {
				char *p = strstr(line, "last_mac_indexed=");
				if (p)
					sscanf(p + 17, "%63s", last);
			}
			fclose(f);
		}
	}
	if (!last[0]) {
		if (!ctx->factory_mac[0])
			mesh_get_factory_mac(ctx, ctx->factory_mac, sizeof(ctx->factory_mac));
		if (mesh_convert_to_la(ctx->factory_mac, base, sizeof(base)) != 0)
			return -1;
		snprintf(last, sizeof(last), "%s", base);
	}

	if (sscanf(last, "%x:%x:%x:%x:%x:%x",
		   &o[0], &o[1], &o[2], &o[3], &o[4], &o[5]) != 6)
		return -1;
	o[3] = (o[3] + 1) % 256;
	snprintf(out, outlen, "%02x:%02x:%02x:%02x:%02x:%02x",
		 o[0], o[1], o[2], o[3], o[4], o[5]);

	{
		char content[96];
		snprintf(content, sizeof(content), "last_mac_indexed=%s\n", out);
		mesh_file_write(last_path, content);
		f = fopen(maclist_path, "a");
		if (f) {
			fprintf(f, "%s_mac=%s\n", devname, out);
			fclose(f);
		}
	}
	(void)ctx;
	return 0;
}

static void apply_bridge_mac(struct mesh_ctx *ctx, const char *br,
			     const char *mac, const char *uci_tuple)
{
	char out[256];
	char *show[] = { "ip", "link", "show", (char *)br, NULL };
	char cur[32] = "";

	if (mesh_run_capture(show, out, sizeof(out)) == 0) {
		char *p = strstr(out, "link/ether ");
		if (p)
			sscanf(p + 11, "%31s", cur);
	}
	if (cur[0] && !strcmp(cur, mac))
		return;
	mesh_log_debug(ctx, "Setting %s MAC to %s", br, mac);
	if (uci_tuple && uci_tuple[0]) {
		char setl[160];
		snprintf(setl, sizeof(setl), "set %s.macaddr='%s'", uci_tuple, mac);
		mesh_uci_batch_line(setl);
	}
	{
		char *a[] = { "ip", "link", "set", (char *)br, "down", NULL };
		mesh_run_capture(a, NULL, 0);
	}
	{
		char *a[] = { "ip", "link", "set", (char *)br, "address", (char *)mac, NULL };
		mesh_run_capture(a, NULL, 0);
	}
	{
		char *a[] = { "ip", "link", "set", (char *)br, "up", NULL };
		mesh_run_capture(a, NULL, 0);
	}
}

int mesh_set_bridge_mac_addresses(struct mesh_ctx *ctx)
{
	char mesh_bridge[64];
	char brmesh_mac[32];
	char status[16];
	char out[4096];
	char *brctl[] = { "brctl", "show", NULL };

	if (ctx->auto_config == 0 &&
	    access(MESH11SD_TMPDIR "/auto_config_test", F_OK) != 0) {
		mesh_log_debug(ctx, "daemon is idling");
		return 0;
	}
	if (ctx->swconfig_mac_skip) {
		mesh_log_debug(ctx, "swconfig: leaving MAC addresses unchanged");
		return 0;
	}

	mesh_log_debug(ctx, "checking mac addresses");
	snprintf(mesh_bridge, sizeof(mesh_bridge), "br-%s", ctx->auto_mesh_network);
	if (mesh_get_indexed_mac(ctx, mesh_bridge, brmesh_mac, sizeof(brmesh_mac)) == -1)
		return 1;
	mesh_wait_for_interface(ctx, mesh_bridge, status, sizeof(status));
	if (!strcmp(status, "down")) {
		mesh_wait_for_interface(ctx, mesh_bridge, status, sizeof(status));
		if (!strcmp(status, "down"))
			return 1;
	}
	{
		char key[96];
		snprintf(key, sizeof(key), "network.%s", ctx->auto_mesh_network);
		apply_bridge_mac(ctx, mesh_bridge, brmesh_mac, key);
		{
			char content[64];
			snprintf(content, sizeof(content), "brmesh_mac=\"%s\"\n", brmesh_mac);
			mesh_file_write(MESH11SD_TMPDIR "/brmesh_mac", content);
		}
	}

	/* member ports: indexed LA except mesh vif uses factory MAC */
	if (mesh_run_capture(brctl, out, sizeof(out)) == 0) {
		char *p = out, *line;
		int hdr = 1;
		while ((line = strsep(&p, "\n")) != NULL) {
			char iface[64];
			char *tok;
			if (hdr) { hdr = 0; continue; }
			/* last field is often an interface name */
			tok = strrchr(line, '\t');
			if (!tok)
				tok = strrchr(line, ' ');
			if (!tok)
				continue;
			while (*tok == ' ' || *tok == '\t')
				tok++;
			sscanf(tok, "%63s", iface);
			if (!strcmp(iface, "no") || !strcmp(iface, "yes") || !iface[0])
				continue;
			{
				char want[32];
				int is_mesh = 0;
				if (ctx->active_mesh_ifname[0] &&
				    !strcmp(iface, ctx->active_mesh_ifname))
					is_mesh = 1;
				if (!strncmp(iface, "m-11s-", 6) || !strncmp(iface, "m-", 2))
					is_mesh = 1;
				if (is_mesh) {
					if (!ctx->factory_mac[0])
						continue;
					snprintf(want, sizeof(want), "%s", ctx->factory_mac);
				} else {
					if (mesh_get_indexed_mac(ctx, iface, want, sizeof(want)) < 0)
						continue;
				}
				apply_bridge_mac(ctx, iface, want, NULL);
			}
		}
	}
	{
		char *a[] = { "ip", "link", "set", mesh_bridge, "up", NULL };
		mesh_run_capture(a, NULL, 0);
	}
	return 0;
}

int mesh_set_root_bridge_priority(struct mesh_ctx *ctx)
{
	char bridge[64];
	char path[160];
	unsigned priority = 8192;
	FILE *f;
	char cur[32] = "";

	snprintf(bridge, sizeof(bridge), "br-%s", ctx->auto_mesh_network);
	if (!strcmp(ctx->detected_state, "portal")) {
		priority = 0;
	} else {
		char hash[72];
		if (!ctx->factory_mac[0])
			mesh_get_factory_mac(ctx, ctx->factory_mac, sizeof(ctx->factory_mac));
		if (mesh_sha256_hex(ctx->factory_mac, hash, sizeof(hash)) == 0) {
			char last4[8];
			snprintf(last4, sizeof(last4), "%.4s", hash + strlen(hash) - 4);
			priority = (unsigned)strtoul(last4, NULL, 16);
		}
	}
	snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/priority", bridge);
	f = fopen(path, "r");
	if (f) {
		if (!fgets(cur, sizeof(cur), f))
			cur[0] = '\0';
		fclose(f);
	}
	{
		char want[16];
		snprintf(want, sizeof(want), "%u", priority);
		if (strncmp(cur, want, strlen(want)) != 0) {
			f = fopen(path, "w");
			if (f) {
				fprintf(f, "%u\n", priority);
				fclose(f);
				mesh_log_info(ctx, "Set %s STP priority to %u (detected_state=%s)",
					      bridge, priority, ctx->detected_state);
			}
		}
	}
	return 0;
}

int mesh_create_vxlan_if(struct mesh_ctx *ctx)
{
	char vxname[32];
	char setl[192];
	unsigned vni = 69;

	if (!ctx->vtun_enable)
		return 0;
	mesh_set_ula_prefix(ctx, "get");
	if (ctx->ula_subnet[0])
		vni = (unsigned)strtoul(ctx->ula_subnet, NULL, 16);
	snprintf(vxname, sizeof(vxname), "vxlan%s", ctx->tun_id[0] ? ctx->tun_id : "69");

	/* network UCI for vtunlan */
	mesh_uci_batch_line("set network.vtunlan=interface");
	mesh_uci_batch_line("set network.vtunlan.proto='static'");
	mesh_uci_batch_line("set network.vtunlan.device='br-tun69'");
	snprintf(setl, sizeof(setl), "set network.vtunlan.ip6hint='%s'", ctx->ula_vxsubnet);
	mesh_uci_batch_line(setl);

	{
		char vni_s[16], group[64];
		char *argv[] = {
			"ip", "link", "add", vxname, "type", "vxlan",
			"id", vni_s, "dstport", "4789", "group", group, NULL
		};
		snprintf(vni_s, sizeof(vni_s), "%u", vni ? vni : 69);
		snprintf(group, sizeof(group), "ff02::%s",
			 ctx->ula_subnet[0] ? ctx->ula_subnet : "1");
		mesh_run_capture(argv, NULL, 0);
	}
	{
		char br[32];
		snprintf(br, sizeof(br), "br-tun%s", ctx->tun_id[0] ? ctx->tun_id : "69");
		{
			char *a[] = { "ip", "link", "add", "name", br, "type", "bridge", NULL };
			mesh_run_capture(a, NULL, 0);
		}
		{
			char *a[] = { "ip", "link", "set", vxname, "master", br, NULL };
			mesh_run_capture(a, NULL, 0);
		}
		{
			char *a[] = { "ip", "link", "set", vxname, "up", NULL };
			mesh_run_capture(a, NULL, 0);
		}
		{
			char *a[] = { "ip", "link", "set", br, "up", NULL };
			mesh_run_capture(a, NULL, 0);
		}
	}
	(void)setl;
	return 0;
}

int mesh_check_vtunnel(struct mesh_ctx *ctx)
{
	char vxname[32];
	char out[256];
	char *argv[] = { "ip", "link", "show", vxname, NULL };

	if (!ctx->vtun_enable)
		return 0;
	snprintf(vxname, sizeof(vxname), "vxlan%s", ctx->tun_id[0] ? ctx->tun_id : "69");
	argv[3] = vxname;
	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		mesh_create_vxlan_if(ctx);
	return 0;
}

int mesh_block_bridge_loops(struct mesh_ctx *ctx)
{
	/* v7: nft table bridge mesh11s + chain mesh_PRE (prerouting -350) */
	char out[256];
	char *list[] = { "nft", "-a", "list", "table", "bridge", "mesh11s", NULL };

	mesh_refresh_bridgemac(ctx);
	if (mesh_run_capture(list, out, sizeof(out)) != 0) {
		char *add[] = { "nft", "add", "table", "bridge", "mesh11s", NULL };
		mesh_run_capture(add, NULL, 0);
		{
			char *c[] = {
				"nft", "add", "chain", "bridge", "mesh11s", "mesh_PRE",
				"{", "type", "filter", "hook", "prerouting",
				"priority", "-350;", "}",
				NULL
			};
			mesh_run_capture(c, NULL, 0);
		}
	}
	return 0;
}


int mesh_move_ethernet_to_vxlan_bridge(struct mesh_ctx *ctx)
{
	char vxlan_bridge[32], mesh_bridge[64];
	char out[4096];
	char *show[] = { "brctl", "show", mesh_bridge, NULL };
	char *p, *line;
	int moved = 0;

	if (!ctx->move_ethernet_to_vxlan_bridge)
		return 0;
	if (ctx->portal_detect == 3)
		return 0;
	if (!ctx->vtun_enable || !ctx->tun_id[0] || !strcmp(ctx->tun_id, "0"))
		return 0;
	snprintf(vxlan_bridge, sizeof(vxlan_bridge), "br-tun%s", ctx->tun_id);
	snprintf(mesh_bridge, sizeof(mesh_bridge), "br-%s", ctx->auto_mesh_network);
	show[2] = mesh_bridge;
	{
		char *chk[] = { "brctl", "show", vxlan_bridge, NULL };
		if (mesh_run_capture(chk, out, sizeof(out)) != 0)
			return 0;
	}
	if (mesh_run_capture(show, out, sizeof(out)) != 0)
		return 0;
	p = out;
	while ((line = strsep(&p, "\n")) != NULL) {
		char port[64];
		char *tok = strrchr(line, '\t');
		if (!tok)
			tok = strrchr(line, ' ');
		if (!tok)
			continue;
		while (*tok == ' ' || *tok == '\t')
			tok++;
		if (sscanf(tok, "%63s", port) != 1 || !port[0])
			continue;
		if (!strncmp(port, "m-11s-", 6) || !strncmp(port, "phy", 3) ||
		    !strncmp(port, "vxradio", 7) || !strncmp(port, "vxlan", 5) ||
		    !strncmp(port, "br-", 3))
			continue;
		if (strncmp(port, "lan", 3) && strncmp(port, "eth", 3) &&
		    strncmp(port, "usb", 3) && strncmp(port, "enx", 3))
			continue;
		{
			char *del[] = { "brctl", "delif", mesh_bridge, port, NULL };
			char *add[] = { "brctl", "addif", vxlan_bridge, port, NULL };
			char *up[] = { "ip", "link", "set", port, "up", NULL };
			mesh_log_debug(ctx, "Moving %s from %s → %s", port, mesh_bridge, vxlan_bridge);
			mesh_run_capture(del, NULL, 0);
			mesh_run_capture(add, NULL, 0);
			mesh_run_capture(up, NULL, 0);
			moved++;
		}
	}
	if (moved > 0)
		mesh_log_notice(ctx, "Moved %d Ethernet port(s) to VXLAN bridge (%s)",
				moved, vxlan_bridge);
	{
		char *up[] = { "ip", "link", "set", vxlan_bridge, "up", NULL };
		mesh_run_capture(up, NULL, 0);
	}
	return 0;
}

int mesh_probe_interface_mac(struct mesh_ctx *ctx)
{
	int i;
	char list[512] = "";

	mesh_get_wiphys(ctx);
	for (i = 0; i < ctx->wiphy_count; i++) {
		char *add[] = {
			"iw", "phy", ctx->wiphys[i], "interface", "add", "probe",
			"type", "mesh", "mesh_id", "probe", NULL
		};
		char out[2048];
		char mac[32] = "";
		char *del[] = { "iw", "dev", "probe", "del", NULL };
		mesh_run_capture(add, NULL, 0);
		{
			char *dev[] = { "iw", "dev", NULL };
			if (mesh_run_capture(dev, out, sizeof(out)) == 0) {
				char *p = strstr(out, "Interface probe");
				if (p) {
					char *a = strstr(p, "addr ");
					if (a)
						sscanf(a + 5, "%31s", mac);
				}
			}
		}
		if (mac[0]) {
			size_t n = strlen(list);
			snprintf(list + n, sizeof(list) - n, "%s%s", n ? " " : "", mac);
		}
		mesh_run_capture(del, NULL, 0);
	}
	mesh_log_debug(ctx, "dsa_mesh_maclist [ %s ]", list);
	return 0;
}
