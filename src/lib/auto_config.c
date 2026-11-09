/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 4: v7 auto_config, backups, mesh_gate_state, restart_mesh.
 */
#include "mesh11sd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int phy_index_hex(const char *phy)
{
	int n = 0;
	const char *p = phy;

	if (!p)
		return 0;
	if (!strncmp(p, "phy", 3))
		p += 3;
	n = atoi(p);
	return n;
}

static void radio_index_str(const char *radio, char *out, size_t outlen)
{
	const char *p = radio;
	int n;

	if (!strncmp(p, "radio", 5))
		p += 5;
	n = atoi(p);
	snprintf(out, outlen, "%x", n);
}


/* From "wireless.<section>.mode='ap'" return section name. */
static int wifi_section_from_mode_line(const char *line, char *sect, size_t sectlen)
{
	const char *dot, *dot2, *eq;

	if (!line || !sect || sectlen == 0)
		return -1;
	sect[0] = '\0';
	dot = strchr(line, '.');
	if (!dot)
		return -1;
	dot2 = strchr(dot + 1, '.');
	eq = strchr(line, '=');
	if (!dot2 || !eq || dot2 > eq)
		return -1;
	{
		size_t n = (size_t)(dot2 - (dot + 1));
		if (n >= sectlen)
			n = sectlen - 1;
		memcpy(sect, dot + 1, n);
		sect[n] = '\0';
	}
	return sect[0] ? 0 : -1;
}


int mesh_backup_defaults(struct mesh_ctx *ctx)
{
	const char *configs[] = {
		"mesh11sd", "wireless", "dhcp", "network", "firewall", "system", NULL
	};
	int i;

	mesh_log_info(ctx, "Initialising state for auto_config");
	for (i = 0; configs[i]; i++) {
		char src[64], dst[96];

		mesh_uci_revert(configs[i]);
		snprintf(src, sizeof(src), "/etc/config/%s", configs[i]);
		snprintf(dst, sizeof(dst), "/etc/config/%s_mesh11sd_default", configs[i]);
		if (access(dst, F_OK) != 0) {
			char *argv[] = { "cp", src, dst, NULL };
			mesh_run_capture(argv, NULL, 0);
		}
	}
	return 0;
}

int mesh_mesh_gate_state(struct mesh_ctx *ctx)
{
	char show[8192];
	char *p, *line;
	char mesh_device[64] = "";
	int ifdisable = (ctx->mesh_gate_enable == 1 || ctx->mesh_gate_enable == 2) ? 0 : 1;

	(void)ifdisable;
	if (ctx->active_mesh_ifname[0]) {
		char filter[128];
		snprintf(filter, sizeof(filter), "%s", ctx->active_mesh_ifname);
		if (mesh_uci_show_filter("wireless", filter, show, sizeof(show)) == 0) {
			p = show;
			while ((line = strsep(&p, " \n")) != NULL) {
				char key[96], val[64];
				char sect[96];
				char *eq = strchr(line, '=');
				if (!eq)
					continue;
				snprintf(sect, sizeof(sect), "%s", line);
				sect[eq - line] = '\0';
				snprintf(key, sizeof(key), "%s.device", sect);
				if (mesh_uci_get(key, val, sizeof(val)) == 0)
					snprintf(mesh_device, sizeof(mesh_device), "%s", val);
				break;
			}
		}
	}

	{
		char *uargv[] = { "uci", "show", "wireless", NULL };
		char raw[16384];
		if (mesh_run_capture(uargv, raw, sizeof(raw)) != 0)
			return 0;
		p = raw;
		while ((line = strsep(&p, "\n")) != NULL) {
			char sect[96], key[128], gate_device[64], setl[160];
			if (!strstr(line, ".mode='ap'") && !strstr(line, ".mode=ap"))
				continue;
			if (wifi_section_from_mode_line(line, sect, sizeof(sect)) != 0)
				continue;
			snprintf(key, sizeof(key), "wireless.%s.device", sect);
			if (mesh_uci_get(key, gate_device, sizeof(gate_device)) != 0)
				continue;
			if (ctx->mesh_gate_enable == 1)
				snprintf(setl, sizeof(setl), "set wireless.%s.disabled='0'", sect);
			else if (ctx->mesh_gate_enable == 0)
				snprintf(setl, sizeof(setl), "set wireless.%s.disabled='1'", sect);
			else if (ctx->mesh_gate_enable == 2 && mesh_device[0] &&
				 !strcmp(gate_device, mesh_device))
				snprintf(setl, sizeof(setl), "set wireless.%s.disabled='1'", sect);
			else
				snprintf(setl, sizeof(setl), "set wireless.%s.disabled='0'", sect);
			mesh_uci_batch_line(setl);
		}
	}
	/* v7: separate vxradio pass — disable vxradio on the mesh radio when
	 * mesh_gate_enable=2 (gates only on non-mesh radios). */
	if (ctx->mesh_gate_enable == 2 && mesh_device[0]) {
		char *uargv[] = { "uci", "show", "wireless", NULL };
		char raw[16384];
		if (mesh_run_capture(uargv, raw, sizeof(raw)) == 0) {
			p = raw;
			while ((line = strsep(&p, "\n")) != NULL) {
				char sect[96], key[128], gate_device[64], setl[160];
				if (!strstr(line, "vtunlan"))
					continue;
				/* wireless.vxradioN.network='vtunlan' */
				if (!strstr(line, ".network="))
					continue;
				{
					char *dot = strchr(line, '.');
					char *dot2;
					if (!dot)
						continue;
					dot2 = strchr(dot + 1, '.');
					if (!dot2)
						continue;
					snprintf(sect, sizeof(sect), "%.*s",
						 (int)(dot2 - (dot + 1)), dot + 1);
				}
				if (strncmp(sect, "vxradio", 7) != 0)
					continue;
				snprintf(key, sizeof(key), "wireless.%s.device", sect);
				if (mesh_uci_get(key, gate_device, sizeof(gate_device)) != 0)
					continue;
				if (!strcmp(gate_device, mesh_device)) {
					snprintf(setl, sizeof(setl),
						 "set wireless.%s.disabled='1'", sect);
					mesh_uci_batch_line(setl);
				}
			}
		}
	}
	return 0;
}


int mesh_apply_mesh_params(struct mesh_ctx *ctx)
{
	char show[4096];
	char *p, *line;
	const char *params[] = {
		"mesh_fwding", "mesh_retry_timeout", "mesh_confirm_timeout",
		"mesh_holding_timeout", "mesh_rssi_threshold",
		"mesh_gate_announcements", "mesh_hwmp_rootmode", NULL
	};

	mesh_get_mesh_iflist(ctx);
	if (mesh_uci_show_filter("mesh11sd", "mesh_params", show, sizeof(show)) != 0)
		return 0;

	for (int i = 0; i < ctx->iflist_count; i++) {
		int pi;
		for (pi = 0; params[pi]; pi++) {
			char key[96], val[64];
			snprintf(key, sizeof(key), "mesh11sd.mesh_params.%s", params[pi]);
			if (mesh_uci_get(key, val, sizeof(val)) != 0 || !val[0])
				continue;
			{
				char *argv[] = {
					"iw", "dev", ctx->iflist[i], "set", "mesh_param",
					(char *)params[pi], val, NULL
				};
				mesh_run_capture(argv, NULL, 0);
			}
		}
	}
	(void)p;
	(void)line;
	return 0;
}

int mesh_restart_mesh(struct mesh_ctx *ctx)
{
	mesh_log_debug(ctx, "restarting mesh");
	mesh_set_txpower_uci(ctx);
	mesh_mesh_gate_state(ctx);
	mesh_mobility_settings(ctx, "config");
	{
		char *argv[] = { "wifi", NULL };
		mesh_run_capture(argv, NULL, 0);
	}
	/* wait_for_mesh: give mesh ifaces time to appear */
	{
		int tries;
		for (tries = 0; tries < 10; tries++) {
			mesh_get_mesh_iflist(ctx);
			if (ctx->iflist_count > 0)
				break;
			sleep(1);
		}
	}
	if (ctx->device[0]) {
		char *argv1[] = { "ip", "link", "set", ctx->device, "arp", "on", NULL };
		char *argv2[] = { "ip", "link", "set", ctx->device, "multicast", "on", NULL };
		mesh_run_capture(argv1, NULL, 0);
		mesh_run_capture(argv2, NULL, 0);
	}
	mesh_mobility_settings(ctx, "runtime");
	mesh_apply_mesh_params(ctx);
	return 0;
}

static int mesh_capable_phy(const char *phy)
{
	/* Stream: "mesh point" can sit past a 8–16KB truncation on Wi-Fi 7 phys */
	return mesh_phy_info_has(phy, "mesh point");
}

int mesh_auto_config(struct mesh_ctx *ctx)
{
	const char *wpad_pkgs[] = {
		"wpad-mbedtls", "wpad-wolfssl", "wpad-openssl",
		"wpad-mesh-mbedtls", "wpad-mesh-wolfssl", "wpad-mesh-openssl", NULL
	};
	const char *dfs_etsi = "AD";
	int meshifenabled = 0;
	int wi, ri;
	char setl[256];
	int ifdisable = (ctx->mesh_gate_enable == 1 || ctx->mesh_gate_enable == 2) ? 0 : 1;

	mesh_log_debug(ctx, "Entering auto config....");

	if (mesh_is_installed("kmod-ath10k-ct") == 0) {
		mesh_log_err(ctx, "Unable to autoconfigure. Please install ath10k NON ct drivers for mesh support");
		sleep(5);
		return -1;
	}

	snprintf(setl, sizeof(setl), "set dhcp.odhcpd.loglevel='%d'", ctx->odhcpd_log_level);
	mesh_uci_batch_line(setl);
	mesh_uci_batch_line("set dhcp.@dnsmasq[0].logfacility='-'");
	mesh_uci_batch_line("set dhcp.@dnsmasq[0].quietdhcp='1'");

	/* vxlan APs if enabled — v7 ~3811 */
	if (ctx->vtun_enable) {
		char show[8192];
		char *p, *line;
		if (mesh_uci_show_filter("wireless", "mode='ap'", show, sizeof(show)) == 0) {
			p = show;
			while ((line = strsep(&p, "\n")) != NULL) {
				char sect[96], device[64], key[128];
				int radio_index;

				/* filter joins matches with spaces; also split on space */
				if (!line[0])
					continue;
				/* each token may be a full wireless.*.mode='ap' line */
				{
					char *tok = line;
					char *sp;
					for (; tok && *tok; tok = sp ? sp + 1 : NULL) {
						sp = strchr(tok, ' ');
						if (sp)
							*sp = '\0';
						if (wifi_section_from_mode_line(tok, sect, sizeof(sect)) != 0) {
							if (!sp)
								break;
							continue;
						}
						snprintf(key, sizeof(key), "wireless.%s.device", sect);
						if (mesh_uci_get(key, device, sizeof(device)) != 0) {
							if (!sp)
								break;
							continue;
						}
						if (strncmp(device, "radio", 5)) {
							if (!sp)
								break;
							continue;
						}
						radio_index = atoi(device + 5);
						snprintf(setl, sizeof(setl),
							 "set wireless.vxradio%d=wifi-iface", radio_index);
						mesh_uci_batch_line(setl);
						snprintf(setl, sizeof(setl),
							 "set wireless.vxradio%d.device='radio%d'",
							 radio_index, radio_index);
						mesh_uci_batch_line(setl);
						snprintf(setl, sizeof(setl),
							 "set wireless.vxradio%d.network='vtunlan'", radio_index);
						mesh_uci_batch_line(setl);
						snprintf(setl, sizeof(setl),
							 "set wireless.vxradio%d.mode='ap'", radio_index);
						mesh_uci_batch_line(setl);
						snprintf(setl, sizeof(setl),
							 "set wireless.vxradio%d.ifname='vxradio%d'",
							 radio_index, radio_index);
						mesh_uci_batch_line(setl);
						snprintf(setl, sizeof(setl),
							 "set wireless.vxradio%d.ssid='%s'",
							 radio_index, ctx->vtun_base_ssid[0] ?
							 ctx->vtun_base_ssid : "VTunnel");
						mesh_uci_batch_line(setl);
						snprintf(setl, sizeof(setl),
							 "set wireless.vxradio%d.encryption='none'", radio_index);
						mesh_uci_batch_line(setl);
						snprintf(setl, sizeof(setl),
							 "set wireless.vxradio%d.disabled='%d'",
							 radio_index, ifdisable);
						mesh_uci_batch_line(setl);
						if (!sp)
							break;
					}
				}
			}
		}
	}

	if (mesh_check_package_any(wpad_pkgs, NULL) != 0) {
		mesh_log_err(ctx, "Unable to autoconfigure. Please install package wpad-mbedtls, wpad-wolfssl, wpad-openssl or an equivalent mesh only version");
		sleep(5);
		return -1;
	}

	mesh_refresh_bridgemac(ctx);
	mesh_get_wiphys(ctx);

	for (ri = 0; ri < ctx->radio_count; ri++) {
		snprintf(setl, sizeof(setl), "set wireless.%s.disabled='0'", ctx->radios[ri]);
		mesh_uci_batch_line(setl);
	}

	if (ctx->wiphy_count == 0) {
		mesh_log_err(ctx, "Unable to autoconfigure. No physical wireless interfaces found");
		return -1;
	}

	for (wi = 0; wi < ctx->wiphy_count; wi++) {
		const char *wiphy = ctx->wiphys[wi];
		int phyindex;
		char phyhex[8];

		if (!mesh_capable_phy(wiphy))
			continue;
		phyindex = phy_index_hex(wiphy);
		snprintf(phyhex, sizeof(phyhex), "%x", phyindex);

		if (ctx->mesh_phy_index[0] && strcmp(phyhex, ctx->mesh_phy_index) != 0) {
			mesh_log_info(ctx, "option mesh_phy_index [ %s ] does not match phyindex [ %s ] - skipping this wiphy",
				      ctx->mesh_phy_index, phyhex);
			continue;
		}

		for (ri = 0; ri < ctx->radio_count; ri++) {
			char radiophy[32];
			char radiohex[8];
			char mesh_ifname[64];
			char band[16];
			char mesh_band[16];
			char ucicountry[16];
			int radio_num;

			if (mesh_radiophy(ctx->radios[ri], radiophy, sizeof(radiophy)) != 0)
				continue;
			if (strcmp(radiophy, wiphy) != 0)
				continue;

			radio_index_str(ctx->radios[ri], radiohex, sizeof(radiohex));
			radio_num = (int)strtol(radiohex, NULL, 16);
			snprintf(mesh_ifname, sizeof(mesh_ifname), "%sr%sp%s",
				 ctx->mesh_basename, radiohex, phyhex);

			snprintf(setl, sizeof(setl), "set wireless.m11s%s=wifi-iface", radiohex);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.m11s%s.device='radio%s'", radiohex, radiohex);
			/* device should be radioN decimal name from ctx->radios */
			snprintf(setl, sizeof(setl), "set wireless.m11s%s.device='%s'", radiohex, ctx->radios[ri]);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.m11s%s.mode='mesh'", radiohex);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.m11s%s.encryption='sae'", radiohex);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.m11s%s.mesh_id='%s'", radiohex, ctx->auto_mesh_id);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.m11s%s.key='%s'", radiohex, ctx->auto_mesh_key);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.m11s%s.network='%s'", radiohex, ctx->auto_mesh_network);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.m11s%s.ifname='%s'", radiohex, mesh_ifname);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.m11s%s.mesh_rssi_threshold='%d'",
				 radiohex, ctx->mesh_rssi_threshold);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.m11s%s.dtim_period='%d'",
				 radiohex, ctx->mesh_dtim_period);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.%s.disabled='0'", ctx->radios[ri]);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.%s.beacon_int='%d'",
				 ctx->radios[ri], ctx->mesh_beacon_interval);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.%s.log_level='3'", ctx->radios[ri]);
			mesh_uci_batch_line(setl);

			snprintf(setl, sizeof(setl), "wireless.%s.country", ctx->radios[ri]);
			if (mesh_uci_get(setl, ucicountry, sizeof(ucicountry)) != 0)
				ucicountry[0] = '\0';
			/* trim to first token like v7 awk '{printf "%s", $1}' */
			{
				char *sp = strchr(ucicountry, ' ');
				if (sp)
					*sp = '\0';
			}
			if (!ctx->country[0] && !ucicountry[0]) {
				snprintf(setl, sizeof(setl), "set wireless.%s.country='%s'",
					 ctx->radios[ri], dfs_etsi);
				mesh_uci_batch_line(setl);
				mesh_log_warn(ctx, "WARNING - country code - not set, so setting to safe default....");
				mesh_log_warn(ctx, "Country code defaulting to [ DFS-ETSI ]....");
				snprintf(ctx->country, sizeof(ctx->country), "%s", dfs_etsi);
			}
			if (ctx->country[0]) {
				snprintf(setl, sizeof(setl), "set wireless.%s.country='%s'",
					 ctx->radios[ri], ctx->country);
				mesh_uci_batch_line(setl);
			}

			snprintf(setl, sizeof(setl), "wireless.%s.band", ctx->radios[ri]);
			if (mesh_uci_get(setl, band, sizeof(band)) != 0)
				band[0] = '\0';
			{
				char *sp = strchr(band, ' ');
				if (sp)
					*sp = '\0';
			}
			mesh_log_debug(ctx, "wireless.%s.band [ %s ]", ctx->radios[ri], band);

			if (!strcmp(ctx->auto_mesh_band, "2g40"))
				snprintf(mesh_band, sizeof(mesh_band), "2g");
			else
				snprintf(mesh_band, sizeof(mesh_band), "%s", ctx->auto_mesh_band);

			if (!strcmp(band, mesh_band) && !meshifenabled) {
				snprintf(setl, sizeof(setl), "set wireless.m11s%s.disabled='0'", radiohex);
				mesh_uci_batch_line(setl);
				if (!ctx->mesh_multi_interfaces_enabled)
					meshifenabled = 1;
				if (!ctx->active_mesh_ifname[0]) {
					snprintf(ctx->active_mesh_ifname,
						 sizeof(ctx->active_mesh_ifname), "%s", mesh_ifname);
					{
						char content[128];
						snprintf(content, sizeof(content),
							 "active_mesh_ifname=\"%s\"\n", mesh_ifname);
						mesh_file_write(MESH11SD_TMPDIR "/active_mesh_ifname", content);
					}
				}
			} else {
				snprintf(setl, sizeof(setl), "set wireless.m11s%s.disabled='1'", radiohex);
				mesh_uci_batch_line(setl);
			}

			if (!strcmp(band, "2g") && !strcmp(ctx->auto_mesh_band, "2g40")) {
				char htmode[32], ht40[32];
				snprintf(setl, sizeof(setl), "wireless.%s.htmode", ctx->radios[ri]);
				if (mesh_uci_get(setl, htmode, sizeof(htmode)) == 0) {
					/* tr "20" "40" style: replace 20 with 40 in mode string */
					char *h;
					snprintf(ht40, sizeof(ht40), "%s", htmode);
					h = strstr(ht40, "20");
					if (h) {
						h[0] = '4';
						h[1] = '0';
					}
					if (ctx->country[0]) {
						snprintf(setl, sizeof(setl), "set wireless.%s.noscan='1'", ctx->radios[ri]);
						mesh_uci_batch_line(setl);
						snprintf(setl, sizeof(setl), "set wireless.%s.htmode='%s'",
							 ctx->radios[ri], ht40);
						mesh_uci_batch_line(setl);
					}
				}
			}

			if (!strcmp(band, "2g") && !strcmp(ctx->portal_channel, "auto")) {
				/* deterministic channel from factory/device mac */
				unsigned char seed = 9;
				int chan = 1;
				const char *mac = ctx->devicemac[0] ? ctx->devicemac : ctx->factory_mac;
				if (mac[0]) {
					char hash[72];
					if (mesh_sha256_hex(mac, hash, sizeof(hash)) == 0) {
						char c = hash[strlen(hash) - 1];
						if (c >= '0' && c <= '9')
							chan = (c - '0') % 13 + 1;
						else if (c >= 'a' && c <= 'f')
							chan = ((c - 'a') + 10) % 13 + 1;
					}
				}
				(void)seed;
				snprintf(setl, sizeof(setl), "set wireless.%s.channel='%d'",
					 ctx->radios[ri], chan);
				mesh_uci_batch_line(setl);
			} else if (!strcmp(band, "2g") &&
				   strcmp(ctx->portal_channel, "auto") &&
				   strcmp(ctx->portal_channel, "default")) {
				int chan = atoi(ctx->portal_channel);
				if (chan > 13)
					chan = 13;
				if (chan <= 0)
					chan = 1;
				snprintf(setl, sizeof(setl), "set wireless.%s.channel='%d'",
					 ctx->radios[ri], chan);
				mesh_uci_batch_line(setl);
			}
			(void)radio_num;
		}
	}

	/* AP ssid suffix + encryption — v7 ~4040-4100 */
	{
		char *p, *line;
		char suffix[8] = "0000";
		int ifindex = 0;
		const char *enc_type = "none";
		const char *vx_enc = "none";
		char *uargv[] = { "uci", "show", "wireless", NULL };
		char raw[16384];

		if (ctx->factory_mac[0] && strlen(ctx->factory_mac) >= 17) {
			snprintf(suffix, sizeof(suffix), "%c%c%c%c",
				 ctx->factory_mac[12], ctx->factory_mac[13],
				 ctx->factory_mac[15], ctx->factory_mac[16]);
		}

		switch (ctx->mesh_gate_encryption) {
		case 1: enc_type = "sae"; break;
		case 2: enc_type = "sae-mixed+aes"; break;
		case 3: enc_type = "psk2+aes"; break;
		case 4: enc_type = "owe"; break;
		default: enc_type = "none"; break;
		}
		switch (ctx->vtun_gate_encryption) {
		case 1: vx_enc = "sae"; break;
		case 2: vx_enc = "sae-mixed+aes"; break;
		case 3: vx_enc = "psk2+aes"; break;
		case 4: vx_enc = "owe"; break;
		default: vx_enc = "none"; break;
		}

		snprintf(setl, sizeof(setl), "set system.@system[0].hostname='meshnode-%s'", suffix);
		mesh_uci_batch_line(setl);
		{
			char *argv[] = { "/sbin/service", "system", "restart", NULL };
			mesh_run_capture(argv, NULL, 0);
		}

		/* First pass: ifname + ssid (v7 walks aplist twice) */
		if (mesh_run_capture(uargv, raw, sizeof(raw)) == 0) {
			p = raw;
			while ((line = strsep(&p, "\n")) != NULL) {
				char sect[96], radio[64], band[16], ssid[64], ifname[64];
				char phy[32], phyhex[8], radiohex[8], key[128];
				int is_vx;

				if (!strstr(line, ".mode='ap'") && !strstr(line, ".mode=ap"))
					continue;
				if (wifi_section_from_mode_line(line, sect, sizeof(sect)) != 0)
					continue;
				is_vx = strstr(sect, "vxradio") != NULL;
				snprintf(key, sizeof(key), "wireless.%s.device", sect);
				if (mesh_uci_get(key, radio, sizeof(radio)) != 0)
					continue;
				mesh_radiophy(radio, phy, sizeof(phy));
				snprintf(phyhex, sizeof(phyhex), "%x", phy_index_hex(phy));
				radio_index_str(radio, radiohex, sizeof(radiohex));
				snprintf(key, sizeof(key), "wireless.%s.band", radio);
				if (mesh_uci_get(key, band, sizeof(band)) != 0)
					band[0] = '\0';
				{
					char *sp = strchr(band, ' ');
					if (sp)
						*sp = '\0';
				}
				snprintf(key, sizeof(key), "wireless.%s.ssid", sect);
				if (mesh_uci_get(key, ssid, sizeof(ssid)) != 0)
					ssid[0] = '\0';
				snprintf(key, sizeof(key), "wireless.%s.ifname", sect);
				if (mesh_uci_get(key, ifname, sizeof(ifname)) != 0 || !ifname[0]) {
					snprintf(ifname, sizeof(ifname), "%s-ap%d",
						 phy[0] ? phy : "phy0", ifindex);
					ifindex++;
					snprintf(setl, sizeof(setl),
						 "set wireless.%s.ifname='%s'", sect, ifname);
					mesh_uci_batch_line(setl);
				}
				if (strcmp(ssid, ctx->vtun_base_ssid) != 0 &&
				    ctx->mesh_gate_base_ssid[0])
					snprintf(ssid, sizeof(ssid), "%s", ctx->mesh_gate_base_ssid);
				if (ctx->ssid_suffix_enable) {
					snprintf(setl, sizeof(setl),
						 "set wireless.%s.ssid='%s-%s-%s-r%sp%s'",
						 sect, ssid, band, suffix, radiohex, phyhex);
				} else {
					snprintf(setl, sizeof(setl),
						 "set wireless.%s.ssid='%s'", sect, ssid);
				}
				mesh_uci_batch_line(setl);
				(void)is_vx;
			}
		}

		/* Second pass: encryption + key */
		if (mesh_run_capture(uargv, raw, sizeof(raw)) == 0) {
			p = raw;
			while ((line = strsep(&p, "\n")) != NULL) {
				char sect[96], key[128];
				int is_vx;

				if (!strstr(line, ".mode='ap'") && !strstr(line, ".mode=ap"))
					continue;
				if (wifi_section_from_mode_line(line, sect, sizeof(sect)) != 0)
					continue;
				is_vx = strstr(sect, "vxradio") != NULL;
				if (!is_vx) {
					snprintf(setl, sizeof(setl),
						 "set wireless.%s.encryption='%s'", sect, enc_type);
					mesh_uci_batch_line(setl);
					if (ctx->mesh_gate_key[0]) {
						snprintf(setl, sizeof(setl),
							 "set wireless.%s.key='%s'",
							 sect, ctx->mesh_gate_key);
						mesh_uci_batch_line(setl);
					}
				} else {
					snprintf(setl, sizeof(setl),
						 "set wireless.%s.encryption='%s'", sect, vx_enc);
					mesh_uci_batch_line(setl);
					if (ctx->vtun_gate_key[0]) {
						snprintf(setl, sizeof(setl),
							 "set wireless.%s.key='%s'",
							 sect, ctx->vtun_gate_key);
						mesh_uci_batch_line(setl);
					}
				}
			}
		}
	}

	mesh_mesh_gate_state(ctx);
	/* v7 ~10925 lives in autostartup; also apply here so auto_config test
	 * (flag created after daemon start) still gets portal IPv4 before wifi. */
	if (ctx->portal_detect != 5)
		mesh_apply_portal_ipv4(ctx);
	/* mobility config (incl. MLO/driver beacon override) before wifi */
	mesh_mobility_settings(ctx, "config");
	mesh_restart_mesh(ctx);
	mesh_log_debug(ctx, "auto config complete....");
	mesh_log_info(ctx, "auto_config: mesh bring-up done (wifi applied)");
	return 0;
}

/* v7 ~11225: set mesh/AP macaddr in UCI before wifi so runtime MAC walk is a no-op. */
int mesh_fixup_wireless_macaddrs(struct mesh_ctx *ctx)
{
	char *p, *line;
	int did = 0;

	if (ctx->macfixup)
		return 0;
	if (!ctx->factory_mac[0])
		mesh_get_factory_mac(ctx, ctx->factory_mac, sizeof(ctx->factory_mac));
	if (!ctx->factory_mac[0])
		return -1;

	mesh_refresh_bridgemac(ctx);
	mesh_get_wiphys(ctx);

	/* Mesh ifaces: factory MAC when unset and commit_all=0 */
	for (int ri = 0; ri < ctx->radio_count; ri++) {
		char radiohex[8], sect[32], key[96], cur[64], ifn[64], setl[160];
		const char *radio = ctx->radios[ri];
		const char *digits = radio;

		if (!strncmp(radio, "radio", 5))
			digits = radio + 5;
		snprintf(radiohex, sizeof(radiohex), "%x", atoi(digits));
		snprintf(sect, sizeof(sect), "m11s%s", radiohex);
		snprintf(key, sizeof(key), "wireless.%s.macaddr", sect);
		if (mesh_uci_get(key, cur, sizeof(cur)) == 0 && cur[0])
			continue;
		if (ctx->commit_all != 0)
			continue;
		/* section must exist */
		snprintf(key, sizeof(key), "wireless.%s", sect);
		if (mesh_uci_get(key, cur, sizeof(cur)) != 0)
			continue;
		snprintf(key, sizeof(key), "wireless.%s.ifname", sect);
		if (mesh_uci_get(key, ifn, sizeof(ifn)) != 0)
			ifn[0] = '\0';
		snprintf(setl, sizeof(setl), "set wireless.%s.macaddr='%s'",
			 sect, ctx->factory_mac);
		mesh_uci_batch_line(setl);
		mesh_log_notice(ctx, "Setting mac address of mesh interface %s to [ %s ]",
				ifn[0] ? ifn : sect, ctx->factory_mac);
		ctx->changed = 1;
		did = 1;
	}

	/* AP ifaces: indexed LA MAC when unset and commit_all=0 (v7 ~11252) */
	{
		char *uargv[] = { "uci", "show", "wireless", NULL };
		char raw[16384];
		if (mesh_run_capture(uargv, raw, sizeof(raw)) == 0) {
			p = raw;
			while ((line = strsep(&p, "\n")) != NULL) {
				char sect[96], key[128], cur[64], ifn[64], want[32], setl[160];
				char *dot, *dot2, *eq;
				if (!strstr(line, ".mode='ap'") && !strstr(line, ".mode=ap"))
					continue;
				dot = strchr(line, '.');
				if (!dot)
					continue;
				dot2 = strchr(dot + 1, '.');
				eq = strchr(line, '=');
				if (!dot2 || !eq || dot2 > eq)
					continue;
				{
					size_t n = (size_t)(dot2 - (dot + 1));
					if (n >= sizeof(sect))
						n = sizeof(sect) - 1;
					memcpy(sect, dot + 1, n);
					sect[n] = '\0';
				}
				if (!sect[0])
					continue;
				snprintf(key, sizeof(key), "wireless.%s.macaddr", sect);
				if (mesh_uci_get(key, cur, sizeof(cur)) == 0 && cur[0])
					continue;
				if (ctx->commit_all != 0)
					continue;
				snprintf(key, sizeof(key), "wireless.%s.ifname", sect);
				if (mesh_uci_get(key, ifn, sizeof(ifn)) != 0 || !ifn[0])
					continue;
				if (mesh_get_indexed_mac(ctx, ifn, want, sizeof(want)) < 0)
					continue;
				snprintf(setl, sizeof(setl), "set wireless.%s.macaddr='%s'",
					 sect, want);
				mesh_uci_batch_line(setl);
				mesh_log_notice(ctx, "Setting mac address of %s to [ %s ]",
						ifn, want);
				/* v7 leaves changed alone for AP, but firstloop always wifi'd.
				 * Mark changed so a non-firstloop macfixup still triggers wifi. */
				ctx->changed = 1;
				did = 1;
			}
		}
	}

	ctx->macfixup = 1;
	(void)did;
	return 0;
}
