/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 6: bridge MAC walk, STP priority, vxlan bring-up, bridge-loop nft.
 */
#include "mesh11sd.h"

#include <ctype.h>
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

static int read_sysfs_mac(const char *ifname, char *mac, size_t maclen)
{
	char path[160];
	FILE *f;
	size_t n;

	if (!ifname || !ifname[0] || !mac || maclen < 18)
		return -1;
	snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(mac, (int)maclen, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	n = strlen(mac);
	while (n > 0 && (mac[n - 1] == '\n' || mac[n - 1] == '\r'))
		mac[--n] = '\0';
	for (n = 0; mac[n]; n++)
		mac[n] = (char)tolower((unsigned char)mac[n]);
	return mac[0] ? 0 : -1;
}

static int mac_equal(const char *a, const char *b)
{
	unsigned oa[6], ob[6];

	if (!a || !b || !*a || !*b)
		return 0;
	if (sscanf(a, "%x:%x:%x:%x:%x:%x",
		   &oa[0], &oa[1], &oa[2], &oa[3], &oa[4], &oa[5]) != 6)
		return 0;
	if (sscanf(b, "%x:%x:%x:%x:%x:%x",
		   &ob[0], &ob[1], &ob[2], &ob[3], &ob[4], &ob[5]) != 6)
		return 0;
	return oa[0] == ob[0] && oa[1] == ob[1] && oa[2] == ob[2] &&
	       oa[3] == ob[3] && oa[4] == ob[4] && oa[5] == ob[5];
}

static void normalize_mac(const char *in, char *out, size_t outlen)
{
	unsigned o[6];

	out[0] = '\0';
	if (!in || sscanf(in, "%x:%x:%x:%x:%x:%x",
			  &o[0], &o[1], &o[2], &o[3], &o[4], &o[5]) != 6)
		return;
	snprintf(out, outlen, "%02x:%02x:%02x:%02x:%02x:%02x",
		 o[0], o[1], o[2], o[3], o[4], o[5]);
}

static int swconfig_bridge(void)
{
	char out[64];

	return mesh_uci_get("network.@switch[0].name", out, sizeof(out)) == 0 &&
	       out[0] != '\0';
}

static int mesh_port_in_bridge(const char *br)
{
	char out[4096];
	char *argv[] = { "brctl", "show", (char *)br, NULL };
	char *p, *line;
	int hdr = 1;

	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return 0;
	p = out;
	while ((line = strsep(&p, "\n")) != NULL) {
		char iface[64];
		char *tok;
		char iwout[512];
		char *iwargv[] = { "iw", "dev", iface, "info", NULL };

		if (hdr) {
			hdr = 0;
			continue;
		}
		tok = strrchr(line, '\t');
		if (!tok)
			tok = strrchr(line, ' ');
		if (!tok)
			continue;
		while (*tok == ' ' || *tok == '\t')
			tok++;
		if (sscanf(tok, "%63s", iface) != 1 || !iface[0])
			continue;
		if (!strcmp(iface, "no") || !strcmp(iface, "yes"))
			continue;
		iwargv[2] = iface;
		if (mesh_run_capture(iwargv, iwout, sizeof(iwout)) == 0 &&
		    strstr(iwout, "type mesh"))
			return 1;
	}
	return 0;
}

/* v7 apply_bridge_mac: set bridge MAC without bouncing wireless ports.
 * Try while UP first; down/up only if needed. Skip change when current matches.
 */
static int apply_bridge_mac(struct mesh_ctx *ctx, const char *br,
			    const char *mac, const char *uci_tuple)
{
	char cur[32] = "";
	char want[32];
	char path[160];
	char config_mac[32] = "";

	if (!br || !br[0] || !mac || !mac[0])
		return 1;
	snprintf(path, sizeof(path), "/sys/class/net/%s", br);
	if (access(path, F_OK) != 0)
		return 1;

	normalize_mac(mac, want, sizeof(want));
	if (!want[0])
		return 1;

	{
		char *up[] = { "ip", "link", "set", (char *)br, "up", NULL };
		mesh_run_capture(up, NULL, 0);
	}

	if (read_sysfs_mac(br, cur, sizeof(cur)) == 0 && mac_equal(cur, want))
		goto uci_only;

	if (swconfig_bridge() && mesh_port_in_bridge(br)) {
		mesh_log_debug(ctx, "swconfig: %s has a mesh port, leaving it up", br);
		goto uci_only;
	}

	mesh_log_debug(ctx, "Setting %s MAC to %s", br, want);
	{
		char *a[] = { "ip", "link", "set", (char *)br, "address", want, NULL };
		mesh_run_capture(a, NULL, 0);
	}
	if (read_sysfs_mac(br, cur, sizeof(cur)) != 0 || !mac_equal(cur, want)) {
		char *dn[] = { "ip", "link", "set", (char *)br, "down", NULL };
		char *ad[] = { "ip", "link", "set", (char *)br, "address", want, NULL };
		char *up[] = { "ip", "link", "set", (char *)br, "up", NULL };
		mesh_run_capture(dn, NULL, 0);
		mesh_run_capture(ad, NULL, 0);
		mesh_run_capture(up, NULL, 0);
		if (read_sysfs_mac(br, cur, sizeof(cur)) != 0 || !mac_equal(cur, want)) {
			mesh_log_debug(ctx, "Failed to set mac [ %s ] on [ %s ]", want, br);
			return 1;
		}
	}

uci_only:
	if (uci_tuple && uci_tuple[0]) {
		char key[160];
		snprintf(key, sizeof(key), "%s.macaddr", uci_tuple);
		if (mesh_uci_get(key, config_mac, sizeof(config_mac)) != 0)
			config_mac[0] = '\0';
		if (!mac_equal(config_mac, want)) {
			char setl[192];
			snprintf(setl, sizeof(setl), "set %s.macaddr='%s'", uci_tuple, want);
			mesh_uci_batch_line(setl);
		}
	}
	return 0;
}

/* v7 member-port MAC walk: try set while UP; down/up only if still wrong.
 * Never log or change when /sys address already matches want.
 */
static void apply_member_mac(struct mesh_ctx *ctx, const char *iface,
			     const char *mac)
{
	char cur[32] = "";
	char want[32];

	if (!iface || !iface[0] || !mac || !mac[0])
		return;
	normalize_mac(mac, want, sizeof(want));
	if (!want[0])
		return;
	if (read_sysfs_mac(iface, cur, sizeof(cur)) == 0 && mac_equal(cur, want))
		return;

	mesh_log_debug(ctx, "Setting %s MAC to %s", iface, want);
	{
		char *a[] = { "ip", "link", "set", (char *)iface, "address", want, NULL };
		mesh_run_capture(a, NULL, 0);
	}
	if (read_sysfs_mac(iface, cur, sizeof(cur)) != 0 || !mac_equal(cur, want)) {
		char *dn[] = { "ip", "link", "set", (char *)iface, "down", NULL };
		char *ad[] = { "ip", "link", "set", (char *)iface, "address", want, NULL };
		char *up[] = { "ip", "link", "set", (char *)iface, "up", NULL };
		mesh_run_capture(dn, NULL, 0);
		mesh_run_capture(ad, NULL, 0);
		mesh_run_capture(up, NULL, 0);
	}
}

int mesh_set_bridge_mac_addresses(struct mesh_ctx *ctx)
{
	char mesh_bridge[64];
	char brmesh_mac[32];
	char status[16];
	char out[4096];
	char cur[32];
	char *brctl[] = { "brctl", "show", NULL };
	char cpe_bridge[] = "br-lan";
	char brcpe_mac[32] = "";
	char vxlan_bridge[32];
	char vxlan_mac[32] = "";
	int have_vxlan = 0;

	if (ctx->auto_config == 0 &&
	    access(MESH11SD_TMPDIR "/auto_config_test", F_OK) != 0) {
		mesh_log_debug(ctx, "daemon is idling");
		return 0;
	}
	/* v7: only skip when swconfig switch present AND swconfig_mac_skip != 0 */
	if (swconfig_bridge() && ctx->swconfig_mac_skip) {
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

	/* Mesh bridge: only change when /sys address differs (v7 guard). */
	if (read_sysfs_mac(mesh_bridge, cur, sizeof(cur)) != 0 ||
	    !mac_equal(cur, brmesh_mac)) {
		char key[96];
		char config_mac[32] = "";

		snprintf(key, sizeof(key), "network.%s.macaddr", ctx->auto_mesh_network);
		if (mesh_uci_get(key, config_mac, sizeof(config_mac)) != 0)
			config_mac[0] = '\0';
		if (!mac_equal(config_mac, brmesh_mac)) {
			char setl[192];
			snprintf(setl, sizeof(setl), "set network.%s.macaddr='%s'",
				 ctx->auto_mesh_network, brmesh_mac);
			mesh_uci_batch_line(setl);
		}
		if (swconfig_bridge() && mesh_port_in_bridge(mesh_bridge)) {
			mesh_log_debug(ctx, "swconfig: %s has a mesh port, leaving it up",
				       mesh_bridge);
		} else {
			char *dn[] = { "ip", "link", "set", mesh_bridge, "down", NULL };
			char *ad[] = { "ip", "link", "set", mesh_bridge, "address",
				       brmesh_mac, NULL };
			char *up[] = { "ip", "link", "set", mesh_bridge, "up", NULL };
			char achieved[32] = "";

			mesh_log_debug(ctx, "Setting %s MAC to %s", mesh_bridge, brmesh_mac);
			mesh_run_capture(dn, NULL, 0);
			mesh_run_capture(ad, NULL, 0);
			mesh_run_capture(up, NULL, 0);
			if (read_sysfs_mac(mesh_bridge, achieved, sizeof(achieved)) == 0 &&
			    mac_equal(achieved, brmesh_mac)) {
				char content[64];
				snprintf(content, sizeof(content),
					 "brmesh_mac=\"%s\"\n", brmesh_mac);
				mesh_file_write(MESH11SD_TMPDIR "/brmesh_mac", content);
			} else {
				mesh_log_debug(ctx, "Failed to set mac [ %s ] on [ %s ]",
					       brmesh_mac, mesh_bridge);
			}
		}
	}

	/* CPE LAN bridge when portal_detect=3 */
	if (ctx->portal_detect == 3) {
		if (mesh_get_indexed_mac(ctx, cpe_bridge, brcpe_mac,
					 sizeof(brcpe_mac)) != -1 &&
		    brcpe_mac[0]) {
			char *up[] = { "ip", "link", "set", cpe_bridge, "up", NULL };
			mesh_run_capture(up, NULL, 0);
			apply_bridge_mac(ctx, cpe_bridge, brcpe_mac, "network.lan");
		}
	}

	/* VXLAN bridge when active */
	have_vxlan = ctx->vtun_enable && ctx->tun_id[0] && strcmp(ctx->tun_id, "0") != 0;
	if (have_vxlan) {
		snprintf(vxlan_bridge, sizeof(vxlan_bridge), "br-tun%s", ctx->tun_id);
		if (mesh_get_indexed_mac(ctx, vxlan_bridge, vxlan_mac,
					 sizeof(vxlan_mac)) >= 0) {
			if (access(MESH11SD_TMPDIR "/vxlan_mac", F_OK) != 0) {
				char content[64];
				snprintf(content, sizeof(content),
					 "vxlan_mac=\"%s\"\n", vxlan_mac);
				mesh_file_write(MESH11SD_TMPDIR "/vxlan_mac", content);
			}
			if (read_sysfs_mac(vxlan_bridge, cur, sizeof(cur)) != 0 ||
			    !mac_equal(cur, vxlan_mac)) {
				char *ad[] = { "ip", "link", "set", vxlan_bridge,
					       "address", vxlan_mac, NULL };
				mesh_log_debug(ctx, "Setting %s MAC to %s",
					       vxlan_bridge, vxlan_mac);
				mesh_run_capture(ad, NULL, 0);
			}
		}
	} else {
		/* v7 returns early before member walk when VXLAN inactive.
		 * Stage 1 still walks members — needed for mesh/eth MACs. */
	}

	/* Load active_mesh_ifname from tmpdir if not in ctx (v7 sources file) */
	if (!ctx->active_mesh_ifname[0] &&
	    access(MESH11SD_TMPDIR "/active_mesh_ifname", R_OK) == 0) {
		FILE *f = fopen(MESH11SD_TMPDIR "/active_mesh_ifname", "r");
		char line[128];
		if (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "active_mesh_ifname=\"");
			if (p)
				sscanf(p + 20, "%63[^\"]", ctx->active_mesh_ifname);
		}
		if (f)
			fclose(f);
	}
	if (!ctx->factory_mac[0] &&
	    access(MESH11SD_TMPDIR "/factory_mac", R_OK) == 0) {
		FILE *f = fopen(MESH11SD_TMPDIR "/factory_mac", "r");
		char line[128];
		if (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "factory_mac=\"");
			if (p)
				sscanf(p + 13, "%31[^\"]", ctx->factory_mac);
			else {
				p = strstr(line, "factory_mac=");
				if (p)
					sscanf(p + 12, "%31s", ctx->factory_mac);
			}
		}
		if (f)
			fclose(f);
	}

	/* member ports: indexed LA except mesh vif uses factory MAC */
	if (mesh_run_capture(brctl, out, sizeof(out)) == 0) {
		char *p = out, *line;
		int hdr = 1;
		while ((line = strsep(&p, "\n")) != NULL) {
			char iface[64];
			char *tok;
			if (hdr) {
				hdr = 0;
				continue;
			}
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
				if (!strncmp(iface, "m-11s-", 6))
					is_mesh = 1;
				if (is_mesh) {
					if (!ctx->factory_mac[0])
						continue;
					normalize_mac(ctx->factory_mac, want, sizeof(want));
					if (!want[0])
						continue;
				} else {
					if (mesh_get_indexed_mac(ctx, iface, want,
								 sizeof(want)) < 0)
						continue;
				}
				apply_member_mac(ctx, iface, want);
			}
		}
	}

	/* Bring bridges back up after any member bounce; re-apply bridge MACs. */
	{
		char *a[] = { "ip", "link", "set", mesh_bridge, "up", NULL };
		mesh_run_capture(a, NULL, 0);
	}
	{
		char *a[] = { "ip", "link", "set", cpe_bridge, "up", NULL };
		mesh_run_capture(a, NULL, 0);
	}
	if (have_vxlan && vxlan_mac[0]) {
		char *a[] = { "ip", "link", "set", vxlan_bridge, "up", NULL };
		mesh_run_capture(a, NULL, 0);
	}
	if (brmesh_mac[0]) {
		char key[96];
		snprintf(key, sizeof(key), "network.%s", ctx->auto_mesh_network);
		apply_bridge_mac(ctx, mesh_bridge, brmesh_mac, key);
	}
	if (ctx->portal_detect == 3 && brcpe_mac[0])
		apply_bridge_mac(ctx, cpe_bridge, brcpe_mac, "network.lan");
	if (have_vxlan && vxlan_mac[0])
		apply_bridge_mac(ctx, vxlan_bridge, vxlan_mac, NULL);
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
