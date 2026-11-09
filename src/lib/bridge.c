/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 6: bridge MAC walk, STP priority, vxlan bring-up, bridge-loop nft.
 */
#include "mesh11sd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

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
	/* v7 create_vxlan_if: ip-full `ip -6 link add ... type vxlan` (NOT netifd
	 * vxlan device — OpenWrt vxlan UCI is buggy). Baby-jumbo: mesh/backhaul
	 * MTU 1600, vxlan MTU 1500. Attach to br-tun$tun_id via brctl addif. */
	char vxname[32], brname[32], device[64];
	char vxmac[32] = "";
	char linklocal[64] = "";
	char group[64];
	char vni_s[16];
	char out[512], status[16];
	char conf[320];
	unsigned vni;
	int retry;
	char *show[] = { "ip", "link", "show", vxname, NULL };

	if (!ctx->vtun_enable)
		return 0;
	snprintf(vxname, sizeof(vxname), "vxlan%s",
		 ctx->tun_id[0] ? ctx->tun_id : "69");
	snprintf(brname, sizeof(brname), "br-tun%s",
		 ctx->tun_id[0] ? ctx->tun_id : "69");
	show[3] = vxname;
	if (mesh_run_capture(show, out, sizeof(out)) == 0)
		return 0; /* already exists */

	mesh_set_ula_prefix(ctx, "get");
	vni = ctx->ula_subnet[0] ? (unsigned)strtoul(ctx->ula_subnet, NULL, 16) : 69;
	if (!vni)
		vni = 69;
	snprintf(vni_s, sizeof(vni_s), "%u", vni);

	/* indexed MAC for vxlan netdev (v7 get_indexed_mac) */
	if (mesh_get_indexed_mac(ctx, vxname, vxmac, sizeof(vxmac)) < 0 && !vxmac[0]) {
		mesh_log_err(ctx, "create_vxlan_if: no indexed mac for %s", vxname);
		return 1;
	}

	if (ctx->portal_detect == 3)
		snprintf(device, sizeof(device), "br-wan");
	else
		snprintf(device, sizeof(device), "br-%s",
			 ctx->auto_mesh_network[0] ? ctx->auto_mesh_network : "lan");

	/* group: override from update_vxlan, else ff02::$tun_id (NOT ula_subnet) */
	if (ctx->vxlan_group[0])
		snprintf(group, sizeof(group), "%s", ctx->vxlan_group);
	else
		snprintf(group, sizeof(group), "ff02::%s",
			 ctx->tun_id[0] ? ctx->tun_id : "69");

	/* link-local on underlay bridge — required by ip -6 vxlan */
	{
		char *a[] = { "ip", "-6", "addr", "show", "dev", device, "scope", "link", NULL };
		if (mesh_run_capture(a, out, sizeof(out)) == 0) {
			char *p = strstr(out, "inet6 ");
			if (p) {
				p += 6;
				sscanf(p, "%63s", linklocal);
				{
					char *sl = strchr(linklocal, '/');
					if (sl)
						*sl = '\0';
				}
			}
		}
	}
	if (!linklocal[0]) {
		mesh_log_err(ctx, "create_vxlan_if: no link-local on %s", device);
		return 1;
	}

	if (!ctx->active_mesh_ifname[0] &&
	    access(MESH11SD_TMPDIR "/active_mesh_ifname", R_OK) == 0) {
		FILE *f = fopen(MESH11SD_TMPDIR "/active_mesh_ifname", "r");
		char line[128];
		if (f && fgets(line, sizeof(line), f)) {
			char *q = strstr(line, "active_mesh_ifname=\"");
			if (q)
				sscanf(q + 20, "%63[^\"]", ctx->active_mesh_ifname);
		}
		if (f)
			fclose(f);
	}

	mesh_log_debug(ctx,
		       "create_vxlan_if [ %s ], id [ %s ], address [ %s ], link_local [ %s ], group [ %s ], device [ %s ]",
		       vxname, vni_s, vxmac, linklocal, group, device);
	snprintf(conf, sizeof(conf),
		 "vxlan_if=\"%s\"; id=\"%s\"; address=\"%s\"; link_local=\"%s\"; group=\"%s\"; device=\"%s\"\n",
		 vxname, vni_s, vxmac, linklocal, group, device);
	mesh_file_write(MESH11SD_TMPDIR "/vxlan_conf", conf);

	/* Exact v7: ip -6 link add $vx address $mac type vxlan id $vni dstport 4789
	 * local $ll group $g dev $device ttl 5 */
	{
		char *argv[] = {
			"ip", "-6", "link", "add", vxname,
			"address", vxmac,
			"type", "vxlan",
			"id", vni_s,
			"dstport", "4789",
			"local", linklocal,
			"group", group,
			"dev", device,
			"ttl", "5",
			NULL
		};
		if (mesh_run_capture(argv, NULL, 0) != 0) {
			mesh_log_err(ctx, "create_vxlan_if: ip -6 link add %s failed", vxname);
			return 1;
		}
	}
	{
		char *up[] = { "ip", "link", "set", vxname, "up", NULL };
		mesh_run_capture(up, NULL, 0);
	}

	for (retry = 0; retry < 16; retry++) {
		mesh_wait_for_interface(ctx, vxname, status, sizeof(status));
		if (!strcmp(status, "down")) {
			char *up[] = { "ip", "link", "set", vxname, "up", NULL };
			mesh_run_capture(up, NULL, 0);
			sleep(1);
			continue;
		}
		if (!strcmp(status, "up") || !strcmp(status, "unknown")) {
			/* baby-jumbo: mesh + underlay 1600, vxlan 1500 */
			if (ctx->active_mesh_ifname[0]) {
				char *m[] = { "ip", "link", "set", ctx->active_mesh_ifname,
					      "mtu", "1600", NULL };
				mesh_run_capture(m, NULL, 0);
			}
			{
				char *m[] = { "ip", "link", "set", device, "mtu", "1600", NULL };
				mesh_run_capture(m, NULL, 0);
			}
			{
				char *m[] = { "ip", "link", "set", vxname, "mtu", "1500", NULL };
				mesh_run_capture(m, NULL, 0);
			}
			{
				char *brshow[] = { "ip", "link", "show", brname, NULL };
				if (mesh_run_capture(brshow, out, sizeof(out)) == 0) {
					char *addif[] = { "brctl", "addif", brname, vxname, NULL };
					mesh_run_capture(addif, NULL, 0);
				}
			}
			break;
		}
		sleep(1);
	}

	/* dhcp.vtunlan portal vs peer (v7) */
	if (!strcmp(ctx->detected_state, "portal") ||
	    ctx->portal_detect == 0 ||
	    (ctx->portal_detect == 1 && !strcmp(ctx->detected_state, "portal"))) {
		mesh_uci_batch_line("set dhcp.vtunlan.dhcpv4='server'");
		mesh_uci_batch_line("set dhcp.vtunlan.dhcpv6='server'");
		mesh_uci_batch_line("set dhcp.vtunlan.ra='server'");
		mesh_uci_batch_line("set dhcp.vtunlan.ignore='0'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='none'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='managed-config'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='other-config'");
		mesh_uci_batch_line("add_list dhcp.vtunlan.ra_flags='managed-config'");
		mesh_uci_batch_line("add_list dhcp.vtunlan.ra_flags='other-config'");
		mesh_uci_batch_line("set dhcp.@dnsmasq[0].rebind_protection='1'");
	} else {
		mesh_uci_batch_line("set dhcp.vtunlan.dhcpv4='disabled'");
		mesh_uci_batch_line("set dhcp.vtunlan.dhcpv6='disabled'");
		mesh_uci_batch_line("set dhcp.vtunlan.ra='disabled'");
		mesh_uci_batch_line("set dhcp.vtunlan.ignore='1'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='managed-config'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='other-config'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='none'");
		mesh_uci_batch_line("add_list dhcp.vtunlan.ra_flags='none'");
	}
	mesh_log_info(ctx, "vxlan_if created");
	/* non-blocking dnsmasq restart (v7) */
	{
		char *dns[] = { "/sbin/service", "dnsmasq", "restart", NULL };
		if (fork() == 0) {
			int dn = open("/dev/null", O_RDWR);
			if (dn >= 0) {
				dup2(dn, 0); dup2(dn, 1); dup2(dn, 2);
				if (dn > 2) close(dn);
			}
			execvp(dns[0], dns);
			_exit(127);
		}
	}
	return 0;
}

int mesh_check_vtunnel(struct mesh_ctx *ctx)
{
	/* v7 check_vtunnel: UCI vtunnel/vtunlan + firewall once, network restart,
	 * then create_vxlan_if (ip-full). Recreate vxlan after netifd wipe. */
	char vxname[32], brname[32], setl[192], out[256], vtun_uci[64] = "";
	int vxlan_missing, recreate = 0;
	const char *proto = "dhcp";
	char *show[] = { "ip", "link", "show", vxname, NULL };

	if (!ctx->vtun_enable)
		return 0;
	if (access(MESH11SD_TMPDIR "/active_mesh_ifname", R_OK) != 0)
		return 0;
	if (access(MESH11SD_TMPDIR "/is_portal", R_OK) != 0)
		return 0;
	if (!ctx->detected_state[0]) {
		FILE *f = fopen(MESH11SD_TMPDIR "/is_portal", "r");
		char line[160];
		if (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "detected_state=\"");
			if (p)
				sscanf(p + 16, "%15[^\"]", ctx->detected_state);
		}
		if (f)
			fclose(f);
	}

	snprintf(vxname, sizeof(vxname), "vxlan%s",
		 ctx->tun_id[0] ? ctx->tun_id : "69");
	snprintf(brname, sizeof(brname), "br-tun%s",
		 ctx->tun_id[0] ? ctx->tun_id : "69");
	show[3] = vxname;
	vxlan_missing = (mesh_run_capture(show, out, sizeof(out)) != 0);

	if (!vxlan_missing) {
		/* v7 check_vtunnel always ends with set_iface_path_cost on br-tun
		 * (enables STP + path costs). Early-return skipped that and left
		 * br-tun STP off — broadcast loops via vxlan can saturate radio1
		 * TX and starve 5g AP RX/DHCP. */
		mesh_set_iface_path_cost(ctx, ctx->vtun_path_cost, brname, vxname);
		return 0;
	}
	if (ctx->commit_all)
		return 0;

	mesh_log_debug(ctx, "check_vtunnel. Vxlan Status [ missing ]");
	mesh_set_ula_prefix(ctx, "get");

	/* Ensure dhcp.vtunlan + firewall once */
	if (mesh_uci_get("dhcp.vtunlan", vtun_uci, sizeof(vtun_uci)) != 0 || !vtun_uci[0]) {
		mesh_log_debug(ctx, "vxtunnel configuration");
		mesh_uci_batch_line("set dhcp.vtunlan=dhcp");
		mesh_uci_batch_line("set dhcp.vtunlan.interface='vtunlan'");
		mesh_uci_batch_line("set dhcp.vtunlan.start='100'");
		mesh_uci_batch_line("set dhcp.vtunlan.limit='150'");
		mesh_uci_batch_line("set dhcp.vtunlan.leasetime='12h'");
		mesh_uci_batch_line("set dhcp.vtunlan.ra_slaac='1'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='managed-config'");
		mesh_uci_batch_line("del_list dhcp.vtunlan.ra_flags='other-config'");
		mesh_uci_batch_line("add_list dhcp.vtunlan.ra_flags='managed-config'");
		mesh_uci_batch_line("add_list dhcp.vtunlan.ra_flags='other-config'");
		mesh_uci_batch_line("set dhcp.vtunlan.ignore='1'");
		mesh_uci_batch_line("set dhcp.vtunlan.ra_default='2'");

		mesh_uci_batch_line("set firewall.vtunlan=zone");
		mesh_uci_batch_line("set firewall.vtunlan.name='vtunlan'");
		mesh_uci_batch_line("set firewall.vtunlan.network='vtunlan'");
		mesh_uci_batch_line("set firewall.vtunlan.input='ACCEPT'");
		mesh_uci_batch_line("set firewall.vtunlan.output='ACCEPT'");
		mesh_uci_batch_line("set firewall.vtunlan.forward='ACCEPT'");

		snprintf(setl, sizeof(setl), "set firewall.dhcptun%s=rule",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dhcptun%s.name='VTUN-DHCP-Renew'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dhcptun%s.src='vtunlan'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dhcptun%s.proto='udp'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dhcptun%s.dest_port='67'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dhcptun%s.target='ACCEPT'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dhcptun%s.family='ipv4'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);

		snprintf(setl, sizeof(setl), "set firewall.dnstun%s=rule",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dnstun%s.name='VTUN-Allow-DNS'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dnstun%s.src='vtunlan'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dnstun%s.proto='udp tcp'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dnstun%s.dest_port='53'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dnstun%s.target='ACCEPT'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set firewall.dnstun%s.family='ipv4'",
			 ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_uci_batch_line(setl);

		mesh_uci_batch_line("set firewall.vxlan=rule");
		mesh_uci_batch_line("set firewall.vxlan.name='Allow-vxlan'");
		mesh_uci_batch_line("set firewall.vxlan.src='lan'");
		mesh_uci_batch_line("set firewall.vxlan.proto='udp'");
		mesh_uci_batch_line("set firewall.vxlan.dest_port='4789'");
		mesh_uci_batch_line("set firewall.vxlan.target='ACCEPT'");
		mesh_uci_batch_line("set firewall.vxlan_fwd=forwarding");
		mesh_uci_batch_line("set firewall.vxlan_fwd.src='vtunlan'");
		mesh_uci_batch_line("set firewall.vxlan_fwd.dest='wan'");
		if (ctx->vtun_isolate == 0) {
			mesh_uci_batch_line("set firewall.lan_tun=forwarding");
			mesh_uci_batch_line("set firewall.lan_tun.src='vtunlan'");
			mesh_uci_batch_line("set firewall.lan_tun.dest='lan'");
		}
		{
			char *fw[] = { "/sbin/service", "firewall", "restart", NULL };
			mesh_run_capture(fw, NULL, 0);
		}
	}

	/* If vtunlan UCI already exists, only recreate the ip-full vxlan netdev
	 * (network reload/restart deletes it). */
	vtun_uci[0] = '\0';
	if (mesh_uci_get("network.vtunlan", vtun_uci, sizeof(vtun_uci)) == 0 &&
	    vtun_uci[0]) {
		mesh_log_debug(ctx, "vxlan%s missing, vtunlan exists, recreating",
			       ctx->tun_id[0] ? ctx->tun_id : "69");
		mesh_create_vxlan_if(ctx);
		recreate = 1;
	}

	if (!recreate) {
		if (ctx->portal_detect == 0 ||
		    (ctx->portal_detect == 1 && !strcmp(ctx->detected_state, "portal")) ||
		    ctx->portal_detect == 3 || ctx->portal_detect == 4)
			proto = "static";
		else
			proto = "dhcp";

		mesh_uci_batch_line("set network.vtunnel=device");
		snprintf(setl, sizeof(setl), "set network.vtunnel.name='%s'", brname);
		mesh_uci_batch_line(setl);
		mesh_uci_batch_line("set network.vtunnel.type='bridge'");
		mesh_uci_batch_line("set network.vtunnel.bridge_empty='1'");
		/* Do NOT add vxlan as a netifd port permanently in a way that
		 * fights ip-full — v7 still lists it so br comes up; attach
		 * live via brctl after create. */
		snprintf(setl, sizeof(setl), "del_list network.vtunnel.ports='%s'", vxname);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "add_list network.vtunnel.ports='%s'", vxname);
		mesh_uci_batch_line(setl);
		mesh_uci_batch_line("set network.vtunlan=interface");
		snprintf(setl, sizeof(setl), "set network.vtunlan.device='%s'", brname);
		mesh_uci_batch_line(setl);
		if (ctx->vtun_ip[0] && strcmp(ctx->vtun_ip, "auto") != 0) {
			snprintf(setl, sizeof(setl), "set network.vtunlan.ipaddr='%s'",
				 ctx->vtun_ip);
			mesh_uci_batch_line(setl);
		}
		snprintf(setl, sizeof(setl), "set network.vtunlan.netmask='%s'",
			 ctx->vtun_mask[0] ? ctx->vtun_mask : "255.255.255.0");
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "set network.vtunlan.proto='%s'", proto);
		mesh_uci_batch_line(setl);
		mesh_uci_batch_line("set network.vtunlan.ip6assign='64'");
		snprintf(setl, sizeof(setl), "set network.vtunlan.ip6hint='%s'",
			 ctx->ula_vxsubnet);
		mesh_uci_batch_line(setl);
		mesh_uci_batch_line("set network.vtunlan.ip6privacy='0'");
		mesh_uci_batch_line("set dhcp.vtunlan.ra_lifetime='3600'");
		mesh_uci_batch_line("set dhcp.vtunlan.ra_mininterval='20'");
		mesh_uci_batch_line("set dhcp.vtunlan.ra_maxinterval='60'");

		{
			char *fw[] = { "/sbin/service", "firewall", "restart", NULL };
			char *nr[] = { "/sbin/service", "network", "restart", NULL };
			mesh_run_capture(fw, NULL, 0);
			mesh_run_capture(nr, NULL, 0);
		}
		{
			char st[16];
			mesh_wait_for_interface(ctx, ctx->device[0] ? ctx->device : "br-lan",
						st, sizeof(st));
		}
		/* network restart wiped any live vxlan — create via ip-full now */
		mesh_create_vxlan_if(ctx);
		{
			char *dns[] = { "/sbin/service", "dnsmasq", "reload", NULL };
			mesh_run_capture(dns, NULL, 0);
		}
		/* v7: set_iface_path_cost after vtun ready */
		mesh_set_iface_path_cost(ctx, ctx->vtun_path_cost, brname, vxname);
	}
	return 0;
}

int mesh_update_vxlan_for_active_portal(struct mesh_ctx *ctx)
{
	/* v7 update_vxlan_for_active_portal: group ff02::$portal_ula_subnet */
	char vxname[32], new_group[64], cur_group[64] = "";
	char out[512];
	char *show[] = { "ip", "link", "show", vxname, NULL };

	if (!ctx->vtun_enable)
		return 0;
	snprintf(vxname, sizeof(vxname), "vxlan%s",
		 ctx->tun_id[0] ? ctx->tun_id : "69");
	show[3] = vxname;

	mesh_get_portal_ula(ctx, "get");
	if (!ctx->portal_ula[0]) {
		mesh_create_vxlan_if(ctx);
		return 0;
	}
	if (!ctx->ula_subnet[0]) {
		/* 4th hextet of portal_ula */
		char *p = ctx->portal_ula;
		int col = 0;
		char hextet[8] = "";
		while (*p && col < 3) {
			if (*p++ == ':')
				col++;
		}
		sscanf(p, "%7[^:]", hextet);
		if (hextet[0])
			snprintf(ctx->ula_subnet, sizeof(ctx->ula_subnet), "%s", hextet);
	}
	snprintf(new_group, sizeof(new_group), "ff02::%s",
		 ctx->ula_subnet[0] ? ctx->ula_subnet : (ctx->tun_id[0] ? ctx->tun_id : "69"));

	if (access(MESH11SD_TMPDIR "/vxlan_conf", R_OK) == 0) {
		FILE *f = fopen(MESH11SD_TMPDIR "/vxlan_conf", "r");
		char line[320];
		if (f && fgets(line, sizeof(line), f)) {
			char *g = strstr(line, "group=\"");
			if (g)
				sscanf(g + 7, "%63[^\"]", cur_group);
		}
		if (f)
			fclose(f);
	}
	if (!cur_group[0] && mesh_run_capture(show, out, sizeof(out)) == 0) {
		/* fall through — parse not critical */
	}
	if (cur_group[0] && !strcmp(cur_group, new_group) &&
	    mesh_run_capture(show, out, sizeof(out)) == 0) {
		mesh_log_debug(ctx, "VXLAN already using correct group %s", new_group);
		return 0;
	}

	mesh_log_info(ctx, "Portal change detected - switching VXLAN to group %s (portal %s)",
		      new_group, ctx->portal_ula);
	if (mesh_run_capture(show, out, sizeof(out)) == 0) {
		char *del[] = { "ip", "link", "del", vxname, NULL };
		mesh_run_capture(del, NULL, 0);
		sleep(2);
	}
	snprintf(ctx->vxlan_group, sizeof(ctx->vxlan_group), "%s", new_group);
	mesh_create_vxlan_if(ctx);
	ctx->vxlan_group[0] = '\0';
	mesh_log_info(ctx, "VXLAN successfully reconfigured for new portal");
	return 0;
}


int mesh_block_bridge_loops(struct mesh_ctx *ctx)
{
	/* v7 block_bridge_loops: table+chain, PLC discovery drop, per-port
	 * own-MAC drop + accept. Re-run every loop — fw4 `service firewall
	 * stop` flushes the entire nft ruleset (known fw4 bug). */
	char out[16384];
	char tableset[16384];
	int rule_status = 0;
	const char *powerline_discovery_mac = "00:b0:52:00:00:01";
	char *list[] = { "nft", "-a", "list", "table", "bridge", "mesh11s", NULL };

	mesh_refresh_bridgemac(ctx);
	if (mesh_run_capture(list, tableset, sizeof(tableset)) != 0) {
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
		tableset[0] = '\0';
		mesh_run_capture(list, tableset, sizeof(tableset));
	}

	/* Single global rule for Qualcomm AR7400 PLC / HomePlug discovery MAC.
	 * Those frames otherwise become phantom mpath entries. */
	if (!strstr(tableset, powerline_discovery_mac)) {
		char *rule[] = {
			"nft", "add", "rule", "bridge", "mesh11s", "mesh_PRE",
			"ether", "saddr", (char *)powerline_discovery_mac,
			"counter", "drop",
			NULL
		};
		if (mesh_run_capture(rule, out, sizeof(out)) != 0)
			rule_status = 1;
		/* refresh listing for subsequent rulecount checks */
		mesh_run_capture(list, tableset, sizeof(tableset));
	}

	/* Bridges: brctl show lines with NF==4 start a bridge block */
	{
		char *brshow[] = { "brctl", "show", NULL };
		char bridges[1024];
		char *bp, *bline;
		char bridge_names[16][64];
		int nbridges = 0, bi;

		bridges[0] = '\0';
		if (mesh_run_capture(brshow, bridges, sizeof(bridges)) == 0) {
			bp = bridges;
			while ((bline = strsep(&bp, "\n")) != NULL) {
				char a[64], b[64], c[64], d[64], e[64];
				int nf = sscanf(bline, "%63s %63s %63s %63s %63s",
						a, b, c, d, e);
				/* header "bridge name ..." skipped; NF==4 => name id stp iface */
				if (nf == 4 && strcmp(a, "bridge") != 0) {
					if (nbridges < 16) {
						snprintf(bridge_names[nbridges],
							 sizeof(bridge_names[0]), "%s", a);
						nbridges++;
					}
				}
			}
		}

		for (bi = 0; bi < nbridges; bi++) {
			const char *device = bridge_names[bi];
			char br_mac[32] = "";
			char stpout[8192];
			char *sp, *sline;
			char *iplink[] = { "ip", "link", "show", "dev", (char *)device, NULL };
			char *stp[] = { "brctl", "showstp", (char *)device, NULL };

			if (mesh_run_capture(iplink, out, sizeof(out)) == 0) {
				char *m = strstr(out, "link/ether ");
				if (m)
					sscanf(m + 11, "%31s", br_mac);
			}
			if (!br_mac[0])
				continue;

			/* v7: get_mesh_iflist (unused in rule body; keep for parity) */
			mesh_get_mesh_iflist(ctx);

			if (mesh_run_capture(stp, stpout, sizeof(stpout)) != 0)
				continue;

			/* Ports: line before "port id" containing "(" */
			{
				char ports[32][64];
				int nports = 0;
				char *lines[256];
				int nlines = 0, li;

				sp = stpout;
				while ((sline = strsep(&sp, "\n")) != NULL && nlines < 256)
					lines[nlines++] = sline;
				for (li = 0; li < nlines; li++) {
					if (!strstr(lines[li], "port id"))
						continue;
					if (li == 0)
						continue;
					/* previous non-empty line with ( */
					{
						int k;
						for (k = li - 1; k >= 0; k--) {
							if (!lines[k][0])
								continue;
							if (strchr(lines[k], '(')) {
								char pname[64];
								if (sscanf(lines[k], "%63s", pname) == 1 &&
								    nports < 32) {
									snprintf(ports[nports],
										 sizeof(ports[0]),
										 "%s", pname);
									nports++;
								}
							}
							break;
						}
					}
				}

				for (li = 0; li < nports; li++) {
					const char *br_iface = ports[li];
					int rulecount = 0;
					const char *q;
					/* recount from live table each time */
					tableset[0] = '\0';
					mesh_run_capture(list, tableset, sizeof(tableset));
					q = tableset;
					while ((q = strstr(q, br_iface)) != NULL) {
						/* word-ish match: preceded by non-alnum/_ or start */
						if (q == tableset ||
						    !(isalnum((unsigned char)q[-1]) || q[-1] == '_' ||
						      q[-1] == '-' || q[-1] == '.')) {
							char after = q[strlen(br_iface)];
							if (!isalnum((unsigned char)after) &&
							    after != '_' && after != '-' &&
							    after != '.')
								rulecount++;
						}
						q += strlen(br_iface);
					}
					if (rulecount >= 2)
						continue;

					/* Drop frames that appear to come from our own bridge MAC */
					{
						char *drop[] = {
							"nft", "add", "rule", "bridge", "mesh11s",
							"mesh_PRE",
							"meta", "iifname", (char *)br_iface,
							"ether", "saddr", br_mac,
							"counter", "drop",
							NULL
						};
						if (mesh_run_capture(drop, out, sizeof(out)) != 0)
							rule_status = 1;
					}
					/* Accept everything else on this interface */
					{
						char *acc[] = {
							"nft", "add", "rule", "bridge", "mesh11s",
							"mesh_PRE",
							"meta", "iifname", (char *)br_iface,
							"counter", "accept",
							NULL
						};
						if (mesh_run_capture(acc, out, sizeof(out)) != 0)
							rule_status = 1;
					}
				}
			}
		}
	}

	if (rule_status > 0) {
		mesh_log_info(ctx,
			      "-------------------------------------------------------------------------------------------------------------------------");
		mesh_log_info(ctx,
			      "******* WARNING ******* Unable to add nftables rule(s) - error code [ %d ]. Is the kmod-nft-bridge package installed?",
			      rule_status);
		mesh_log_info(ctx,
			      "******* WARNING ******* A severe mesh bridge-loop storm is likely to occur if any non-mesh backhaul segments are used!");
		mesh_log_info(ctx,
			      "-------------------------------------------------------------------------------------------------------------------------");
	}
	mesh_refresh_bridgemac(ctx);
	return 0;
}


/* v7 pin_active_portal_arp_clear — drop MRP ARP pin when active portal is MBP */
int mesh_pin_active_portal_arp_clear(struct mesh_ctx *ctx)
{
	char *flush[] = {
		"nft", "flush", "chain", "bridge", "mesh11s", "mesh_ARP", NULL
	};
	char *del[] = {
		"nft", "delete", "chain", "bridge", "mesh11s", "mesh_ARP", NULL
	};
	char *list[] = {
		"nft", "list", "chain", "bridge", "mesh11s", "mesh_ARP", NULL
	};
	char out[256];

	unlink(MESH11SD_TMPDIR "/arp_portal_pin");
	if (mesh_run_capture(list, out, sizeof(out)) == 0) {
		mesh_run_capture(flush, NULL, 0);
		mesh_run_capture(del, NULL, 0);
	}
	(void)ctx;
	return 0;
}

/* v7 pin_active_portal_arp — drop gateway ARP on mesh ifaces from any MAC
 * except the active MRP (and its bridge MAC when distinct). Lets every
 * portal run DHCP without clients flapping between portals. */
int mesh_pin_active_portal_arp(struct mesh_ctx *ctx)
{
	char mesh_bridge[64];
	char portal_ula[128] = "";
	char portal_mac[32] = "";
	char own_mac[32] = "";
	char bridge_mac[32] = "";
	char gw_ip[32] = "";
	char pin_sig[384];
	char prev_sig[384] = "";
	char out[4096];
	char path[160];
	FILE *f;
	int i;
	int is_self_portal = 0;

	/* Load is_portal overlay into detected_state if file present */
	if (access(MESH11SD_TMPDIR "/is_portal", R_OK) == 0) {
		f = fopen(MESH11SD_TMPDIR "/is_portal", "r");
		if (f) {
			char line[256];
			if (fgets(line, sizeof(line), f)) {
				char *p = strstr(line, "detected_state=\"");
				if (p) {
					char ds[32] = "";
					sscanf(p + 16, "%31[^\"]", ds);
					if (ds[0])
						snprintf(ctx->detected_state,
							 sizeof(ctx->detected_state), "%s", ds);
				}
			}
			fclose(f);
		}
	}

	/* MBP: gateway is upstream router — do not pin */
	if (ctx->portal_detect == 4 ||
	    (!strcmp(ctx->portal_type, "MBP") &&
	     strcmp(ctx->detected_state, "portal") != 0)) {
		mesh_pin_active_portal_arp_clear(ctx);
		mesh_log_notice(ctx,
				"ARP pin: not applied, active portal is an MBP (gateway is the upstream router)");
		return 0;
	}

	snprintf(mesh_bridge, sizeof(mesh_bridge), "br-%s", ctx->auto_mesh_network);

	if (access(MESH11SD_TMPDIR "/active_portal", R_OK) != 0) {
		mesh_log_info(ctx, "ARP pin: no active_portal file");
		return 0;
	}
	f = fopen(MESH11SD_TMPDIR "/active_portal", "r");
	if (f) {
		char line[512];
		if (fgets(line, sizeof(line), f)) {
			char *p;
			p = strstr(line, "portal_ula=\"");
			if (p)
				sscanf(p + 12, "%127[^\"]", portal_ula);
			p = strstr(line, "portal_mac=\"");
			if (p)
				sscanf(p + 12, "%31[^\"]", portal_mac);
		}
		fclose(f);
	}

	if (access(MESH11SD_TMPDIR "/factory_mac", R_OK) == 0) {
		f = fopen(MESH11SD_TMPDIR "/factory_mac", "r");
		if (f) {
			char line[256];
			if (fgets(line, sizeof(line), f)) {
				char *p = strstr(line, "factory_mac=\"");
				if (p)
					sscanf(p + 13, "%31[^\"]", own_mac);
			}
			fclose(f);
		}
	}
	if (!own_mac[0] && ctx->factory_mac[0])
		snprintf(own_mac, sizeof(own_mac), "%s", ctx->factory_mac);

	if (strlen(portal_mac) != 17 &&
	    !strcmp(ctx->detected_state, "portal") &&
	    strlen(own_mac) == 17)
		snprintf(portal_mac, sizeof(portal_mac), "%s", own_mac);

	if (strlen(portal_mac) != 17) {
		mesh_log_notice(ctx, "ARP pin: active_portal has no portal_mac");
		return 0;
	}

	is_self_portal = (!strcmp(ctx->detected_state, "portal") ||
			  (own_mac[0] && !strcasecmp(portal_mac, own_mac)));

	if (is_self_portal) {
		snprintf(path, sizeof(path), "/sys/class/net/%s/address", mesh_bridge);
		f = fopen(path, "r");
		if (f) {
			if (fgets(bridge_mac, sizeof(bridge_mac), f)) {
				char *nl = strchr(bridge_mac, '\n');
				if (nl)
					*nl = '\0';
			}
			fclose(f);
		}
		/* Prefer .1 address on mesh bridge */
		{
			char *argv[] = {
				"ip", "-4", "addr", "show", "dev", mesh_bridge, NULL
			};
			if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
				char *p = out, *line;
				while ((line = strsep(&p, "\n")) != NULL) {
					char ip[32], *slash;
					if (!strstr(line, "inet "))
						continue;
					if (sscanf(line, " inet %31s", ip) != 1 &&
					    sscanf(line, "inet %31s", ip) != 1)
						continue;
					slash = strchr(ip, '/');
					if (slash)
						*slash = '\0';
					if (strlen(ip) >= 2 &&
					    ip[strlen(ip) - 2] == '.' &&
					    ip[strlen(ip) - 1] == '1') {
						snprintf(gw_ip, sizeof(gw_ip), "%s", ip);
						break;
					}
				}
			}
		}
		if (!gw_ip[0]) {
			char key[96];
			snprintf(key, sizeof(key), "network.%s.ipaddr",
				 ctx->auto_mesh_network);
			mesh_uci_get(key, gw_ip, sizeof(gw_ip));
		}
	} else {
		if (portal_ula[0]) {
			char *argv[] = { "ip", "-6", "neigh", NULL };
			if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
				char *p = out, *line;
				while ((line = strsep(&p, "\n")) != NULL) {
					char *ll;
					if (!strstr(line, portal_ula))
						continue;
					ll = strstr(line, "lladdr ");
					if (!ll)
						continue;
					sscanf(ll + 7, "%31s", bridge_mac);
					if (strlen(bridge_mac) == 17)
						break;
					bridge_mac[0] = '\0';
				}
			}
		}
		{
			char *argv[] = {
				"ip", "-4", "route", "show", "default", "dev",
				mesh_bridge, NULL
			};
			if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
				char *via = strstr(out, "via ");
				if (via)
					sscanf(via + 4, "%31s", gw_ip);
			}
		}
		if (!gw_ip[0]) {
			char key[96];
			snprintf(key, sizeof(key), "network.%s.ipaddr",
				 ctx->auto_mesh_network);
			mesh_uci_get(key, gw_ip, sizeof(gw_ip));
		}
	}

	if (bridge_mac[0] && !strcasecmp(bridge_mac, portal_mac))
		bridge_mac[0] = '\0';

	{
		char canon[32];
		/* mesh_is_ipv4addr_valid: 1 = valid, 0 = invalid (same as CLI) */
		if (!mesh_is_ipv4addr_valid(gw_ip, canon, sizeof(canon))) {
			mesh_log_notice(ctx,
					"ARP pin: no gateway IPv4 for active portal [ %s ]",
					portal_mac);
			return 0;
		}
		snprintf(gw_ip, sizeof(gw_ip), "%s", canon);
	}

	mesh_get_mesh_iflist(ctx);
	if (ctx->iflist_count <= 0) {
		mesh_log_info(ctx, "ARP pin: no mesh interface");
		return 0;
	}

	{
		char ifs[256] = "";
		size_t off = 0;
		for (i = 0; i < ctx->iflist_count; i++) {
			int n = snprintf(ifs + off, sizeof(ifs) - off, "%s%s",
					 off ? " " : "", ctx->iflist[i]);
			if (n < 0 || (size_t)n >= sizeof(ifs) - off)
				break;
			off += (size_t)n;
		}
		snprintf(pin_sig, sizeof(pin_sig), "v2 %s %s %s %s",
			 gw_ip, portal_mac, bridge_mac, ifs);
	}

	f = fopen(MESH11SD_TMPDIR "/arp_portal_pin", "r");
	if (f) {
		if (fgets(prev_sig, sizeof(prev_sig), f)) {
			char *nl = strchr(prev_sig, '\n');
			if (nl)
				*nl = '\0';
		}
		fclose(f);
	}

	if (!strcmp(pin_sig, prev_sig)) {
		char *list[] = {
			"nft", "list", "chain", "bridge", "mesh11s", "mesh_ARP", NULL
		};
		if (mesh_run_capture(list, out, sizeof(out)) != 0 ||
		    !strstr(out, portal_mac))
			prev_sig[0] = '\0';
	}
	if (!strcmp(pin_sig, prev_sig))
		return 0;

	{
		char *tbl[] = { "nft", "list", "table", "bridge", "mesh11s", NULL };
		if (mesh_run_capture(tbl, out, sizeof(out)) != 0) {
			char *add[] = {
				"nft", "add", "table", "bridge", "mesh11s", NULL
			};
			mesh_run_capture(add, NULL, 0);
		}
	}
	{
		char *addc[] = {
			"nft", "add", "chain", "bridge", "mesh11s", "mesh_ARP",
			"{", "type", "filter", "hook", "prerouting",
			"priority", "-400;", "}",
			NULL
		};
		mesh_run_capture(addc, NULL, 0);
	}
	{
		char *flush[] = {
			"nft", "flush", "chain", "bridge", "mesh11s", "mesh_ARP", NULL
		};
		mesh_run_capture(flush, NULL, 0);
	}

	for (i = 0; i < ctx->iflist_count; i++) {
		const char *mesh_if = ctx->iflist[i];
		int rc;

		if (strlen(bridge_mac) == 17) {
			char *reply[] = {
				"nft", "add", "rule", "bridge", "mesh11s", "mesh_ARP",
				"meta", "iifname", (char *)mesh_if,
				"ether", "type", "arp",
				"arp", "operation", "reply",
				"arp", "saddr", "ip", gw_ip,
				"ether", "saddr", "!=", portal_mac,
				"ether", "saddr", "!=", bridge_mac,
				"counter", "drop",
				NULL
			};
			rc = mesh_run_capture(reply, out, sizeof(out));
			if (rc != 0) {
				mesh_log_notice(ctx, "ARP pin failed on [ %s ]: %s",
						mesh_if, out);
				return 0;
			}
			{
				char *req[] = {
					"nft", "add", "rule", "bridge", "mesh11s", "mesh_ARP",
					"meta", "iifname", (char *)mesh_if,
					"ether", "type", "arp",
					"arp", "operation", "request",
					"arp", "saddr", "ip", gw_ip,
					"arp", "saddr", "ether", "!=", portal_mac,
					"arp", "saddr", "ether", "!=", bridge_mac,
					"counter", "drop",
					NULL
				};
				mesh_run_capture(req, NULL, 0);
			}
		} else {
			char *reply[] = {
				"nft", "add", "rule", "bridge", "mesh11s", "mesh_ARP",
				"meta", "iifname", (char *)mesh_if,
				"ether", "type", "arp",
				"arp", "operation", "reply",
				"arp", "saddr", "ip", gw_ip,
				"ether", "saddr", "!=", portal_mac,
				"counter", "drop",
				NULL
			};
			rc = mesh_run_capture(reply, out, sizeof(out));
			if (rc != 0) {
				mesh_log_notice(ctx, "ARP pin failed on [ %s ]: %s",
						mesh_if, out);
				return 0;
			}
			{
				char *req[] = {
					"nft", "add", "rule", "bridge", "mesh11s", "mesh_ARP",
					"meta", "iifname", (char *)mesh_if,
					"ether", "type", "arp",
					"arp", "operation", "request",
					"arp", "saddr", "ip", gw_ip,
					"arp", "saddr", "ether", "!=", portal_mac,
					"counter", "drop",
					NULL
				};
				mesh_run_capture(req, NULL, 0);
			}
		}
	}

	mesh_file_write(MESH11SD_TMPDIR "/arp_portal_pin", pin_sig);
	{
		char ifs[256] = "";
		size_t off = 0;
		for (i = 0; i < ctx->iflist_count; i++) {
			int n = snprintf(ifs + off, sizeof(ifs) - off, "%s%s",
					 off ? " " : "", ctx->iflist[i]);
			if (n < 0 || (size_t)n >= sizeof(ifs) - off)
				break;
			off += (size_t)n;
		}
		mesh_log_notice(ctx,
				"ARP pin: replies for [ %s ] on mesh [ %s ] accepted from portal [ %s ] bridge [ %s ]",
				gw_ip, ifs, portal_mac, bridge_mac);
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
