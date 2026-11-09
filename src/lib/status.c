/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 10: full v7-faithful mesh11sd status JSON (apmond-style json_fmt).
 */
#include "mesh11sd.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int mesh_get_node_type_code(struct mesh_ctx *ctx, char *out, size_t outlen)
{
	const char *code = "N/A";

	switch (ctx->portal_detect) {
	case 0:
		code = "MRP";
		break;
	case 1:
		if (!strcmp(ctx->detected_state, "portal"))
			code = "MRP";
		else if (!strcmp(ctx->detected_state, "peer"))
			code = "MPE";
		else
			code = "N/A";
		break;
	case 3:
		code = "CPE";
		break;
	case 4:
		code = "MBP";
		break;
	case 5:
		code = "TPE";
		break;
	default:
		code = "N/A";
		break;
	}
	snprintf(out, outlen, "%s", code);
	return 0;
}

int mesh_get_mesh_params_dump(const char *iface, char *out, size_t outlen)
{
	char *dump[] = { "iw", "dev", (char *)iface, "mesh_param", "dump", NULL };
	char buf[8192];
	char *list_argv[] = { "iw", "dev", (char *)iface, "get", "mesh_param", NULL };
	char list[4096];

	out[0] = '\0';
	if (mesh_run_capture(dump, buf, sizeof(buf)) == 0 && buf[0]) {
		snprintf(out, outlen, "%s", buf);
		return 0;
	}
	/* fallback: enumerate get mesh_param list */
	if (mesh_run_capture(list_argv, list, sizeof(list)) != 0)
		return 1;
	{
		char *p = list, *line;
		size_t used = 0;
		while ((line = strsep(&p, "\n")) != NULL) {
			char *dash = strstr(line, " - ");
			char name[64], val[128], one[256];
			char *garg[] = { "iw", "dev", (char *)iface, "get", "mesh_param",
					 name, NULL };
			if (!dash)
				continue;
			sscanf(dash + 3, "%63s", name);
			if (mesh_run_capture(garg, val, sizeof(val)) != 0)
				continue;
			{
				char *nl = strchr(val, '\n');
				if (nl)
					*nl = '\0';
			}
			snprintf(one, sizeof(one), "%s = %s\n", name, val);
			if (used + strlen(one) + 1 >= outlen)
				break;
			memcpy(out + used, one, strlen(one) + 1);
			used += strlen(one);
		}
	}
	return out[0] ? 0 : 1;
}

static void emit_mesh_param_fields(const char *params_dump, int *emitted)
{
	char buf[8192];
	char *p, *line;

	snprintf(buf, sizeof(buf), "%s", params_dump);
	p = buf;
	while ((line = strsep(&p, "\n")) != NULL) {
		char key[64] = "", val[128] = "";
		char *eq;
		if (!line[0])
			continue;
		eq = strstr(line, " = ");
		if (!eq)
			continue;
		*eq = '\0';
		sscanf(line, "%63s", key);
		sscanf(eq + 3, "%127s", val);
		if (!key[0])
			continue;
		if (*emitted)
			fputs(",\n", stdout);
		{
			char esc_k[128], esc_v[256];
			mesh_json_str(key, esc_k, sizeof(esc_k));
			mesh_json_str(val, esc_v, sizeof(esc_v));
			printf("      \"%s\": \"%s\"", esc_k, esc_v);
		}
		(*emitted)++;
	}
}

static int read_devstatus_field(const char *iface, const char *field, char *out, size_t outlen)
{
	char cmd[160];
	char buf[4096];
	FILE *f;
	char *p;

	out[0] = '\0';
	snprintf(cmd, sizeof(cmd), "devstatus %s 2>/dev/null", iface);
	f = popen(cmd, "r");
	if (!f)
		return 1;
	buf[0] = '\0';
	while (fgets(buf + strlen(buf), (int)(sizeof(buf) - strlen(buf)), f))
		;
	pclose(f);
	p = strstr(buf, field);
	if (!p)
		return 1;
	p = strchr(p, ':');
	if (!p)
		return 1;
	sscanf(p + 1, " \"%127[^\"]\"", out);
	if (!out[0]) {
		long v = 0;
		if (sscanf(p + 1, " %ld", &v) == 1)
			snprintf(out, outlen, "%ld", v);
	}
	return out[0] ? 0 : 1;
}

static void emit_peers(struct mesh_ctx *ctx, const char *iface)
{
	char out[8192];
	char *argv[] = { "iw", "dev", (char *)iface, "mpath", "dump", NULL };
	char *lines[128];
	int n = 0, i;
	char buf[8192];
	char *p, *line;

	fputs("      \"peers\": {\n", stdout);
	if (mesh_run_capture(argv, out, sizeof(out)) != 0) {
		fputs("      }", stdout);
		return;
	}
	snprintf(buf, sizeof(buf), "%s", out);
	p = buf;
	while ((line = strsep(&p, "\n")) != NULL) {
		if (!line[0] || strstr(line, "DEST") || strstr(line, "dest"))
			continue;
		if (n < 128)
			lines[n++] = line;
	}
	for (i = 0; i < n; i++) {
		char *toks[16];
		int nt = 0;
		char tmp[512], *bp, *t;
		char peermac[32], next_hop[32];
		int metric = 0, hops = 0, pchg = 0, seq = 0;
		char flags[16] = "";
		int air = 0;

		snprintf(tmp, sizeof(tmp), "%s", lines[i]);
		bp = tmp;
		while (nt < 16 && (t = strsep(&bp, " \t")) != NULL)
			if (*t)
				toks[nt++] = t;
		if (nt < 5)
			continue;
		snprintf(peermac, sizeof(peermac), "%s", toks[0]);
		snprintf(next_hop, sizeof(next_hop), "%s", toks[1]);
		metric = atoi(toks[4]);
		if (nt > 10)
			hops = atoi(toks[10]);
		if (nt > 11)
			pchg = atoi(toks[11]);
		if (nt > 3)
			seq = atoi(toks[3]);
		if (nt > 9)
			snprintf(flags, sizeof(flags), "%s", toks[9]);
		mesh_calc_airtime_metric(ctx, peermac, &air, &hops);
		printf("        \"%s\": {\n", peermac);
		mesh_json_field("          ", "next_hop", next_hop, 0);
		mesh_json_field_int("          ", "hop_count", hops, 0);
		mesh_json_field_int("          ", "path_change_count", pchg, 0);
		mesh_json_field_int("          ", "airtime_metric", air, 0);
		mesh_json_field_int("          ", "mpath_metric", metric, 0);
		mesh_json_field_int("          ", "sequence_number", seq, 0);
		mesh_json_field("          ", "hwmp_flags_bitmask", flags, 1);
		printf("        }%s\n", (i + 1 < n) ? "," : "");
	}
	fputs("      }", stdout);
}

static void emit_stations(struct mesh_ctx *ctx, const char *iface, const char *devicemac)
{
	char out[8192];
	char *mpp[] = { "iw", "dev", (char *)iface, "mpp", "dump", NULL };
	char entries[128][96];
	int n = 0, i;
	char *p, *line;
	char *iwdev[] = { "iw", "dev", NULL };
	char iwout[4096];

	(void)ctx;
	/* AP stations + mpp dump */
	if (mesh_run_capture(iwdev, iwout, sizeof(iwout)) == 0) {
		char *save = iwout;
		char cur_if[64] = "";
		int is_ap = 0;
		while ((line = strsep(&save, "\n")) != NULL) {
			if (sscanf(line, "Interface %63s", cur_if) == 1) {
				is_ap = 0;
				continue;
			}
			if (strstr(line, "type AP"))
				is_ap = 1;
			if (is_ap && cur_if[0]) {
				char *st[] = { "iw", "dev", cur_if, "station", "dump", NULL };
				char st_out[4096];
				if (mesh_run_capture(st, st_out, sizeof(st_out)) == 0) {
					char *sp = st_out, *sl;
					while ((sl = strsep(&sp, "\n")) != NULL) {
						char mac[32];
						if (sscanf(sl, "Station %31s", mac) == 1 && n < 128) {
							snprintf(entries[n], sizeof(entries[n]),
								 "%s/%s", mac, devicemac);
							n++;
						}
					}
				}
				cur_if[0] = '\0';
				is_ap = 0;
			}
		}
	}
	if (mesh_run_capture(mpp, out, sizeof(out)) == 0) {
		p = out;
		while ((line = strsep(&p, "\n")) != NULL) {
			char sta[32], proxy[32];
			if (!line[0] || strstr(line, "DEST") || strstr(line, "dest"))
				continue;
			if (sscanf(line, "%31s %31s", sta, proxy) >= 2 && n < 128) {
				snprintf(entries[n], sizeof(entries[n]), "%s/%s", sta, proxy);
				n++;
			}
		}
	}
	mesh_json_field_int("      ", "active_stations", n, 0);
	fputs("      \"stations\": {\n", stdout);
	for (i = 0; i < n; i++) {
		char sta[48], proxy[48];
		char *slash = strchr(entries[i], '/');
		if (!slash)
			continue;
		*slash = '\0';
		snprintf(sta, sizeof(sta), "%.47s", entries[i]);
		snprintf(proxy, sizeof(proxy), "%.47s", slash + 1);
		printf("        \"%s\": {\n", sta);
		mesh_json_field("          ", "proxy_node", proxy, 1);
		printf("        }%s\n", (i + 1 < n) ? "," : "");
	}
	fputs("      }", stdout);
}

static void emit_layer2(struct mesh_ctx *ctx)
{
	char show[4096];
	char *p, *line;
	char bridges[32][64];
	int nb = 0, bi;

	(void)ctx;
	fputs("  \"layer2_connections\": {\n", stdout);
	if (mesh_uci_show_filter("network", "type='bridge'", show, sizeof(show)) == 0) {
		p = show;
		while ((line = strsep(&p, "\n")) != NULL) {
			char sect[96], *eq, name[64], key[128];
			eq = strchr(line, '=');
			if (!eq)
				continue;
			snprintf(sect, sizeof(sect), "%s", line);
			sect[eq - line] = '\0';
			/* network.deviceX=device */
			snprintf(key, sizeof(key), "%s.name", sect);
			if (mesh_uci_get(key, name, sizeof(name)) != 0)
				continue;
			if (nb < 32)
				snprintf(bridges[nb++], sizeof(bridges[0]), "%s", name);
		}
	}
	for (bi = 0; bi < nb; bi++) {
		char *stp[] = { "brctl", "showstp", bridges[bi], NULL };
		char *macs[] = { "brctl", "showmacs", bridges[bi], NULL };
		char stp_out[4096], mac_out[8192];
		char ports[64][2][64]; /* iface, port */
		int np = 0;
		int first = 1;

		printf("    \"%s\": {", bridges[bi]);
		if (mesh_run_capture(stp, stp_out, sizeof(stp_out)) == 0) {
			char *sp = stp_out, *sl;
			while ((sl = strsep(&sp, "\n")) != NULL) {
				char iface[64], port[32];
				if (!strchr(sl, '(') || !strchr(sl, ')'))
					continue;
				if (sscanf(sl, "%63s (%31[^)])", iface, port) >= 2 && np < 64) {
					snprintf(ports[np][0], 64, "%s", iface);
					snprintf(ports[np][1], 64, "%s", port);
					np++;
				}
			}
		}
		if (mesh_run_capture(macs, mac_out, sizeof(mac_out)) == 0) {
			char *sp = mac_out, *sl;
			int hdr = 1;
			while ((sl = strsep(&sp, "\n")) != NULL) {
				int portnum = 0;
				char mac[32], islocal[8];
				int pi;
				if (hdr) { hdr = 0; continue; }
				if (sscanf(sl, "%d %31s %7s", &portnum, mac, islocal) < 3)
					continue;
				if (!strcmp(islocal, "yes"))
					continue;
				for (pi = 0; pi < np; pi++) {
					if (atoi(ports[pi][1]) != portnum)
						continue;
					if (!first)
						fputs(",", stdout);
					first = 0;
					printf("\n      \"%s\": \"%s\"", mac, ports[pi][0]);
					break;
				}
			}
		}
		if (!first)
			fputc('\n', stdout);
		printf("    }%s\n", (bi + 1 < nb) ? "," : "");
	}
	fputs("  }\n", stdout);
}

static void read_procd_status(char *out, size_t outlen)
{
	FILE *f = popen("/etc/init.d/mesh11sd status 2>/dev/null", "r");
	snprintf(out, outlen, "unknown");
	if (!f)
		return;
	if (fgets(out, (int)outlen, f)) {
		size_t n = strlen(out);
		while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
			out[--n] = '\0';
	}
	pclose(f);
}

static void read_txpower(struct mesh_ctx *ctx, char *out, size_t outlen)
{
	int i;
	out[0] = '\0';
	mesh_get_mesh_iflist(ctx);
	for (i = 0; i < ctx->iflist_count; i++) {
		char *argv[] = { "iwinfo", NULL };
		char buf[4096];
		char *p;
		/* status must not block: never sleep-wait for a down/dormant iface */
		if (!mesh_iface_is_up(ctx->iflist[i]))
			continue;
		if (mesh_run_capture(argv, buf, sizeof(buf)) != 0)
			break;
		p = strstr(buf, ctx->iflist[i]);
		if (!p)
			continue;
		p = strstr(p, "Tx-Power:");
		if (p)
			sscanf(p + 9, "%63s", out);
		break;
	}
}

int mesh_cli_status(struct mesh_ctx *ctx)
{
	char procd[64];
	char node_type[16];
	char txpower[64] = "";
	char loop_interval[32] = "";
	char checksum[80] = "";
	int i;
	int any_iface = 0;

	mesh_ctx_load_setup(ctx);
	mesh_get_mesh_iflist(ctx);
	/* airtime_metric needs active_mesh_ifname (daemon writes tmpdir) */
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
	if (access(MESH11SD_TMPDIR "/devicemac", R_OK) == 0) {
		FILE *f = fopen(MESH11SD_TMPDIR "/devicemac", "r");
		char line[256];
		if (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "device=\"");
			if (p)
				sscanf(p + 8, "%63[^\"]", ctx->device);
			p = strstr(line, "devicemac=\"");
			if (p)
				sscanf(p + 11, "%31[^\"]", ctx->devicemac);
		}
		if (f)
			fclose(f);
	}
	if (!ctx->factory_mac[0])
		mesh_get_factory_mac(ctx, ctx->factory_mac, sizeof(ctx->factory_mac));
	if (access(MESH11SD_TMPDIR "/is_portal", R_OK) == 0) {
		FILE *f = fopen(MESH11SD_TMPDIR "/is_portal", "r");
		char line[256];
		if (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "detected_state=\"");
			if (p)
				sscanf(p + 16, "%15[^\"]", ctx->detected_state);
		}
		if (f)
			fclose(f);
	}
	if (access(MESH11SD_TMPDIR "/active_portal", R_OK) == 0) {
		FILE *f = fopen(MESH11SD_TMPDIR "/active_portal", "r");
		char line[256];
		if (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "portal_ula=\"");
			if (p)
				sscanf(p + 12, "%63[^\"]", ctx->portal_ula);
			p = strstr(line, "portal_mac=\"");
			if (p)
				sscanf(p + 12, "%31[^\"]", ctx->portal_mac);
			p = strstr(line, "ula_subnet=\"");
			if (p)
				sscanf(p + 12, "%7[^\"]", ctx->ula_subnet);
		}
		if (f)
			fclose(f);
	}
	read_procd_status(procd, sizeof(procd));
	mesh_get_node_type_code(ctx, node_type, sizeof(node_type));
	read_txpower(ctx, txpower, sizeof(txpower));
	/* v7: checksum=$(sha256sum /usr/sbin/mesh11sd | awk '{printf "%s", $1}') */
	{
		char sumout[128] = "";
		char *sargv[] = { "sha256sum", "/usr/sbin/mesh11sd", NULL };
		checksum[0] = '\0';
		if (mesh_run_capture(sargv, sumout, sizeof(sumout)) == 0)
			sscanf(sumout, "%79s", checksum);
		if (!checksum[0]) {
			char *self[] = { "sha256sum", "/proc/self/exe", NULL };
			if (mesh_run_capture(self, sumout, sizeof(sumout)) == 0)
				sscanf(sumout, "%79s", checksum);
		}
		if (!checksum[0])
			snprintf(checksum, sizeof(checksum), "unknown");
	}
	if (!strcmp(procd, "running") &&
	    access(MESH11SD_TMPDIR "/loop_interval", R_OK) == 0) {
		FILE *f = fopen(MESH11SD_TMPDIR "/loop_interval", "r");
		char line[128];
		if (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "loop_interval=\"");
			if (p)
				sscanf(p + 15, "%31[^\"]", loop_interval);
		}
		if (f)
			fclose(f);
	}

	puts("{");
	puts("  \"setup\": {");
	mesh_json_field("    ", "version", MESH11SD_VERSION, 0);
	mesh_json_field("    ", "checksum", checksum, 0);
	mesh_json_field_int("    ", "enabled", ctx->enabled, 0);
	mesh_json_field("    ", "procd_status", procd, 0);
	mesh_json_field("    ", "node_type", node_type, 0);
	mesh_json_field("    ", "label_mac", ctx->factory_mac, 0);
	mesh_json_field_int("    ", "portal_detect", ctx->portal_detect, 0);
	mesh_json_field_int("    ", "portal_detect_threshold", ctx->portal_detect_threshold, 0);
	mesh_json_field("    ", "portal_channel", ctx->portal_channel, 0);
	mesh_json_field_int("    ", "portal_use_default_ipv4", ctx->portal_use_default_ipv4, 0);
	mesh_json_field_int("    ", "channel_tracking_checkinterval",
			    ctx->channel_tracking_checkinterval, 0);
	mesh_json_field_int("    ", "mesh_mac_forced_forwarding",
			    ctx->mesh_mac_forced_forwarding, 0);
	mesh_json_field_int("    ", "gateway_proxy_arp", ctx->gateway_proxy_arp, 0);
	mesh_json_field_int("    ", "reboot_on_error", ctx->reboot_on_error, 0);
	mesh_json_field_int("    ", "stop_on_error", ctx->stop_on_error, 0);
	mesh_json_field_int("    ", "watchdog_nonvolatile_log",
			    ctx->watchdog_nonvolatile_log, 0);
	mesh_json_field("    ", "mesh_basename", ctx->mesh_basename, 0);
	mesh_json_field_int("    ", "auto_config", ctx->auto_config, 0);
	mesh_json_field("    ", "auto_mesh_network", ctx->auto_mesh_network, 0);
	mesh_json_field("    ", "auto_mesh_band", ctx->auto_mesh_band, 0);
	if (!ctx->mesh_driver[0] && ctx->wiphy_count > 0) {
		char path[160], dline[128];
		FILE *df;
		snprintf(path, sizeof(path), "/sys/class/ieee80211/%s/device/uevent",
			 ctx->wiphys[0]);
		df = fopen(path, "r");
		if (df) {
			while (fgets(dline, sizeof(dline), df)) {
				if (!strncmp(dline, "DRIVER=", 7)) {
					char *nl = strchr(dline + 7, '\n');
					if (nl)
						*nl = '\0';
					snprintf(ctx->mesh_driver, sizeof(ctx->mesh_driver),
						 "%s", dline + 7);
					break;
				}
			}
			fclose(df);
		}
	}
	if (!ctx->mesh_driver[0] && ctx->active_mesh_ifname[0]) {
		char show[2048], radio[32] = "", phy[32] = "";
		if (mesh_uci_show_filter("wireless", ctx->active_mesh_ifname,
					 show, sizeof(show)) == 0) {
			char *pp = show, *line;
			while ((line = strsep(&pp, "\n")) != NULL) {
				char *dot, *dot2, key[128];
				size_t n;
				if (!strstr(line, ".ifname="))
					continue;
				dot = strchr(line, '.');
				dot2 = dot ? strchr(dot + 1, '.') : NULL;
				if (!dot2)
					continue;
				n = (size_t)(dot2 - line);
				{
					char sect[96];
					if (n >= sizeof(sect))
						n = sizeof(sect) - 1;
					memcpy(sect, line, n);
					sect[n] = '\0';
					snprintf(key, sizeof(key), "%s.device", sect);
					if (mesh_uci_get(key, radio, sizeof(radio)) == 0)
						mesh_radiophy(radio, phy, sizeof(phy));
				}
				break;
			}
		}
		if (phy[0]) {
			char path[160], dline[128];
			FILE *df;
			snprintf(path, sizeof(path),
				 "/sys/class/ieee80211/%s/device/uevent", phy);
			df = fopen(path, "r");
			if (df) {
				while (fgets(dline, sizeof(dline), df)) {
					if (!strncmp(dline, "DRIVER=", 7)) {
						char *nl = strchr(dline + 7, '\n');
						if (nl)
							*nl = '\0';
						snprintf(ctx->mesh_driver,
							 sizeof(ctx->mesh_driver),
							 "%s", dline + 7);
						break;
					}
				}
				fclose(df);
			}
		}
	}
	mesh_json_field("    ", "mesh_driver", ctx->mesh_driver, 0);
	if (ctx->mesh_phy_index[0])
		mesh_json_field("    ", "mesh_phy_index", ctx->mesh_phy_index, 0);
	mesh_json_field_int("    ", "mesh_multi_interfaces",
			    ctx->mesh_multi_interfaces_enabled, 0);
	mesh_json_field("    ", "auto_mesh_id", ctx->auto_mesh_id, 0);
	mesh_json_field_int("    ", "mesh_gate_enable", ctx->mesh_gate_enable, 0);
	mesh_json_field_int("    ", "swconfig_mac_skip", ctx->swconfig_mac_skip, 0);
	mesh_json_field_int("    ", "mesh_leechmode_enable", ctx->mesh_gate_only, 0);
	mesh_json_field_int("    ", "mesh_node_mobility_level",
			    ctx->mesh_node_mobility_level, 0);
	if (access(MESH11SD_TMPDIR "/mesh_mobility", R_OK) == 0) {
		mesh_json_field_int("    ", "use_default_beacon_interval",
				    ctx->use_default_beacon_interval, 0);
		mesh_json_field_int("    ", "mesh_beacon_interval",
				    ctx->mesh_beacon_interval, 0);
		mesh_json_field_int("    ", "mesh_dtim_period", ctx->mesh_dtim_period, 0);
	}
	mesh_json_field_int("    ", "mesh_gate_encryption", ctx->mesh_gate_encryption, 0);
	mesh_json_field("    ", "mesh_backhaul_led", ctx->mesh_backhaul_led, 0);
	mesh_json_field_int("    ", "vtun_enable", ctx->vtun_enable, 0);
	if (ctx->vtun_enable) {
		mesh_json_field("    ", "tun_id", ctx->tun_id, 0);
		mesh_json_field("    ", "vtun_ip", ctx->vtun_ip, 0);
		mesh_json_field("    ", "vtun_mask", ctx->vtun_mask, 0);
		mesh_json_field_int("    ", "vtun_gate_encryption",
				    ctx->vtun_gate_encryption, 0);
		mesh_json_field("    ", "vtun_base_ssid", ctx->vtun_base_ssid, 0);
		mesh_json_field_int("    ", "vtun_path_cost", ctx->vtun_path_cost, 0);
		mesh_json_field_int("    ", "vtun_isolate", ctx->vtun_isolate, 0);
		mesh_json_field_int("    ", "move_ethernet_to_vxlan_bridge",
				    ctx->move_ethernet_to_vxlan_bridge, 0);
	}
	mesh_json_field("    ", "txpower", txpower, 0);
	mesh_json_field_int("    ", "mesh_path_cost", ctx->mesh_path_cost, 0);
	mesh_json_field_int("    ", "mesh_path_stabilisation",
			    ctx->mesh_path_stabilisation, 0);
	mesh_json_field_int("    ", "reactive_path_stabilisation_threshold",
			    ctx->reactive_path_stabilisation_threshold, 0);
	mesh_json_field_int("    ", "checkinterval", ctx->checkinterval, 0);
	mesh_json_field_int("    ", "loop_interval_delta", ctx->loop_interval_delta, 0);
	mesh_json_field("    ", "loop_interval", loop_interval, 0);
	mesh_json_field_int("    ", "interface_timeout", ctx->interface_timeout, 0);
	mesh_json_field_int("    ", "ssid_suffix_enable", ctx->ssid_suffix_enable, 0);
	/* apmond_* intentionally omitted from Stage 1 port */
	mesh_json_field_int("    ", "odhcpd_log_level", ctx->odhcpd_log_level, 0);
	if (ctx->portal_detect == 3)
		mesh_json_field("    ", "cpe_mode", ctx->cpe_mode, 0);
	mesh_json_field_int("    ", "debuglevel", ctx->debuglevel, 1);
	puts("  },");

	fputs("  \"mesh_interfaces\": {\n", stdout);
	for (i = 0; i < ctx->iflist_count; i++) {
		char params[8192];
		char meshconfig[96] = "";
		char show[4096];
		char device[32] = "", channel[16] = "", meshid[64] = "";
		char tx_packets[32] = "", tx_bytes[32] = "";
		char rx_packets[32] = "", rx_bytes[32] = "";
		int peers = 0;
		int emitted = 0;
		char *p, *line;

		if (!mesh_iface_is_up(ctx->iflist[i]))
			continue;
		if (mesh_get_mesh_params_dump(ctx->iflist[i], params, sizeof(params)) != 0)
			continue;

		if (any_iface)
			fputs(",\n", stdout);
		any_iface = 1;
		printf("    \"%s\": {\n", ctx->iflist[i]);
		emit_mesh_param_fields(params, &emitted);
		if (emitted)
			fputs(",\n", stdout);

		if (mesh_uci_show_filter("wireless", ctx->iflist[i], show, sizeof(show)) == 0) {
			p = show;
			while ((line = strsep(&p, "\n")) != NULL) {
				char *eq, *dot2;
				if (!strstr(line, ".ifname="))
					continue;
				eq = strchr(line, '=');
				if (!eq)
					continue;
				dot2 = strchr(strchr(line, '.') + 1, '.');
				if (!dot2)
					continue;
				{
					size_t n = (size_t)(dot2 - line);
					if (n >= sizeof(meshconfig))
						n = sizeof(meshconfig) - 1;
					memcpy(meshconfig, line, n);
					meshconfig[n] = '\0';
				}
				break;
			}
		}
		if (!meshconfig[0]) {
			mesh_json_field("      ", "interface",
					"ERROR: mesh interface is unnamed in wireless config", 0);
			mesh_json_field("      ", "error",
					" interface must be named to see further status information", 1);
			fputs("\n    }", stdout);
			continue;
		}
		{
			char key[128];
			snprintf(key, sizeof(key), "%s.device", meshconfig);
			mesh_uci_get(key, device, sizeof(device));
			snprintf(key, sizeof(key), "%s.mesh_id", meshconfig);
			mesh_uci_get(key, meshid, sizeof(meshid));
			if (device[0]) {
				snprintf(key, sizeof(key), "wireless.%s.channel", device);
				mesh_uci_get(key, channel, sizeof(channel));
			}
		}
		mesh_json_field("      ", "mesh_id", meshid, 0);
		mesh_json_field("      ", "device", device, 0);
		mesh_json_field("      ", "channel", channel, 0);
		read_devstatus_field(ctx->iflist[i], "tx_packets", tx_packets, sizeof(tx_packets));
		read_devstatus_field(ctx->iflist[i], "tx_bytes", tx_bytes, sizeof(tx_bytes));
		read_devstatus_field(ctx->iflist[i], "rx_packets", rx_packets, sizeof(rx_packets));
		read_devstatus_field(ctx->iflist[i], "rx_bytes", rx_bytes, sizeof(rx_bytes));
		mesh_json_field("      ", "tx_packets", tx_packets, 0);
		mesh_json_field("      ", "tx_bytes", tx_bytes, 0);
		mesh_json_field("      ", "rx_packets", rx_packets, 0);
		mesh_json_field("      ", "rx_bytes", rx_bytes, 0);
		mesh_json_field("      ", "this_node", ctx->factory_mac, 0);
		if (ctx->portal_mac[0] || ctx->portal_ula[0]) {
			char mg[48];
			mesh_json_field("      ", "active_portal", ctx->portal_mac, 0);
			mesh_json_field("      ", "portal_ula", ctx->portal_ula, 0);
			snprintf(mg, sizeof(mg), "ff02::%s",
				 ctx->ula_subnet[0] ? ctx->ula_subnet : "0");
			mesh_json_field("      ", "multicast_group", mg, 0);
		}
		{
			char mout[4096];
			char *ma[] = { "iw", "dev", ctx->iflist[i], "mpath", "dump", NULL };
			if (mesh_run_capture(ma, mout, sizeof(mout)) == 0) {
				char *mp = mout, *ml;
				int hdr = 1;
				while ((ml = strsep(&mp, "\n")) != NULL) {
					if (hdr) { hdr = 0; continue; }
					if (ml[0])
						peers++;
				}
			}
			mesh_json_field_int("      ", "active_peers", peers, 0);
		}
		emit_peers(ctx, ctx->iflist[i]);
		fputs(",\n", stdout);
		emit_stations(ctx, ctx->iflist[i],
			      ctx->devicemac[0] ? ctx->devicemac : ctx->factory_mac);
		fputs("\n    }", stdout);
	}
	fputs("\n  },\n", stdout);
	emit_layer2(ctx);
	puts("}");
	return 0;
}
