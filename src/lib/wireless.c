/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int mesh_get_mesh_iflist(struct mesh_ctx *ctx)
{
	char out[4096];
	char *argv[] = { "iw", "dev", NULL };
	char *p, *line;
	char iface[64];

	ctx->iflist_count = 0;
	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return -1;
	p = out;
	while ((line = strsep(&p, "\n")) != NULL) {
		char *ip;
		if (!(ip = strstr(line, "Interface ")))
			continue;
		ip += 10;
		while (*ip == ' ')
			ip++;
		sscanf(ip, "%63s", iface);
		{
			char info[1024];
			char *iargv[] = { "iw", "dev", iface, "info", NULL };
			if (mesh_run_capture(iargv, info, sizeof(info)) != 0)
				continue;
			if (strstr(info, "type mesh") || strstr(info, "type Mesh") ||
			    strstr(info, "\ttype mesh")) {
				if (ctx->iflist_count < MESH11SD_MAX_IFACES) {
					snprintf(ctx->iflist[ctx->iflist_count],
						 sizeof(ctx->iflist[0]), "%s", iface);
					ctx->iflist_count++;
				}
			}
		}
	}
	return 0;
}

int mesh_wait_for_interface(struct mesh_ctx *ctx, const char *ifname,
			    char *status_out, size_t status_len)
{
	int i;
	char out[512];
	char *argv[] = { "ip", "link", "show", (char *)ifname, NULL };

	snprintf(status_out, status_len, "down");
	for (i = 0; i < ctx->interface_timeout; i++) {
		if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
			if (strstr(out, "state UP") || strstr(out, "state UNKNOWN")) {
				snprintf(status_out, status_len, "up");
				return 0;
			}
		}
		sleep(1);
	}
	mesh_log_debug(ctx, "%s is not up - giving up for now.", ifname);
	return -1;
}

int mesh_radiophy(const char *radio_or_section, char *phy_out, size_t phy_len)
{
	char section[128];
	char phy[64], path[256];
	char base_path[256], fullpath[512], phy_dir[576];
	char listing[1024];
	char *slash;
	int phy_count = 0;
	char only[32] = "";

	phy_out[0] = '\0';
	if (!radio_or_section || !radio_or_section[0])
		return -1;
	if (!strncmp(radio_or_section, "wireless.", 9))
		snprintf(section, sizeof(section), "%s", radio_or_section);
	else
		snprintf(section, sizeof(section), "wireless.%s", radio_or_section);

	{
		char check[8];
		if (mesh_uci_get(section, check, sizeof(check)) != 0)
			return -1;
	}
	snprintf(phy, sizeof(phy), "%s.phy", section);
	if (mesh_uci_get(phy, phy_out, phy_len) == 0 && phy_out[0])
		return 0;
	{
		char path_opt[160];
		snprintf(path_opt, sizeof(path_opt), "%s.path", section);
		if (mesh_uci_get(path_opt, path, sizeof(path)) != 0 || !path[0])
			return -1;
	}

	/* strip +N */
	snprintf(base_path, sizeof(base_path), "%s", path);
	slash = strchr(base_path, '+');
	if (slash)
		*slash = '\0';

	if (!strncmp(base_path, "platform/", 9))
		snprintf(fullpath, sizeof(fullpath), "/sys/devices/%s", base_path);
	else
		snprintf(fullpath, sizeof(fullpath), "/sys/devices/platform/%s", base_path);

	snprintf(phy_dir, sizeof(phy_dir), "%s/ieee80211", fullpath);
	{
		char *lsargv[] = { "ls", phy_dir, NULL };
		if (mesh_run_capture(lsargv, listing, sizeof(listing)) != 0)
			return -1;
	}
	{
		char *p = listing, *tok;
		while ((tok = strsep(&p, " \n\t")) != NULL) {
			if (!strncmp(tok, "phy", 3)) {
				phy_count++;
				snprintf(only, sizeof(only), "%s", tok);
			}
		}
	}
	if (phy_count == 1) {
		snprintf(phy_out, phy_len, "%s", only);
		return 0;
	}
	if (phy_count > 1) {
		/* take first phy for now (v7 has more complex multi-phy pick) */
		snprintf(phy_out, phy_len, "%s", only);
		return 0;
	}
	return -1;
}

int mesh_get_wiphys(struct mesh_ctx *ctx)
{
	char out[4096];
	char *argv[] = { "iw", "list", NULL };
	char show[8192];
	char *p, *line;
	int i;

	ctx->wiphy_count = 0;
	ctx->radio_count = 0;
	mesh_mkdir_p(MESH11SD_TMPDIR "/wiphys");
	mesh_mkdir_p(MESH11SD_TMPDIR "/radios");

	if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
		p = out;
		while ((line = strsep(&p, "\n")) != NULL) {
			char *w;
			if (!(w = strstr(line, "Wiphy ")))
				continue;
			w += 6;
			while (*w == ' ')
				w++;
			if (ctx->wiphy_count < MESH11SD_MAX_RADIOS) {
				sscanf(w, "%15s", ctx->wiphys[ctx->wiphy_count]);
				ctx->wiphy_count++;
			}
		}
		/* reverse order as v7 */
		for (i = 0; i < ctx->wiphy_count / 2; i++) {
			char tmp[16];
			snprintf(tmp, sizeof(tmp), "%s", ctx->wiphys[i]);
			snprintf(ctx->wiphys[i], sizeof(ctx->wiphys[0]), "%s",
				 ctx->wiphys[ctx->wiphy_count - 1 - i]);
			snprintf(ctx->wiphys[ctx->wiphy_count - 1 - i],
				 sizeof(ctx->wiphys[0]), "%s", tmp);
		}
	}

	if (mesh_uci_show_filter("wireless", "wifi-device", show, sizeof(show)) == 0) {
		p = show;
		while ((line = strsep(&p, " \n")) != NULL) {
			char *dot = strchr(line, '.');
			char *eq = strchr(line, '=');
			if (!dot || !eq)
				continue;
			*eq = '\0';
			dot++;
			if (ctx->radio_count < MESH11SD_MAX_RADIOS) {
				snprintf(ctx->radios[ctx->radio_count],
					 sizeof(ctx->radios[0]), "%s", dot);
				ctx->radio_count++;
			}
		}
	}

	for (i = 0; i < ctx->wiphy_count; i++) {
		char info[8192];
		char *iargv[] = { "iw", "phy", ctx->wiphys[i], "info", NULL };
		char path[192];
		int mesh_capable = 1;
		int total_ifaces = 1;

		if (mesh_run_capture(iargv, info, sizeof(info)) == 0) {
			if (strstr(info, "mesh point"))
				mesh_capable = 0; /* v7: 0 means capable (grep -w -q exit) */
			else
				mesh_capable = 1;
			{
				char *ti = strstr(info, "total <=");
				if (ti)
					sscanf(ti, "total <= %d", &total_ifaces);
			}
		}
		snprintf(path, sizeof(path), "%s/wiphys/%s", MESH11SD_TMPDIR, ctx->wiphys[i]);
		{
			char content[256];
			snprintf(content, sizeof(content),
				 "wiphy=%s; mesh_capable=%d; total_ifaces=%d\n",
				 ctx->wiphys[i], mesh_capable, total_ifaces);
			mesh_file_write(path, content);
		}
		if (mesh_capable)
			mesh_log_info(ctx, "WARNING - radios on wiphy [ %s ] do not support mesh interfaces.",
				      ctx->wiphys[i]);
		mesh_log_debug(ctx, "Max number of VIFs on [ %s ] is [ %d ]",
			       ctx->wiphys[i], total_ifaces);
	}
	return 0;
}

int mesh_get_valid_channels(char *out, size_t outlen)
{
	char raw[8192];
	char *argv[] = { "iw", "list", NULL };
	char *p, *line;
	size_t used = 0;

	out[0] = '\0';
	if (mesh_run_capture(argv, raw, sizeof(raw)) != 0)
		return -1;
	p = raw;
	while ((line = strsep(&p, "\n")) != NULL) {
		char *br;
		int ch;
		if (!strstr(line, "MHz [") || strstr(line, "disabled") ||
		    strstr(line, "radar detection"))
			continue;
		br = strchr(line, '[');
		if (!br)
			continue;
		if (sscanf(br, "[%d]", &ch) == 1) {
			int n = snprintf(out + used, outlen - used, "%s%d",
					 used ? " " : "", ch);
			if (n < 0 || (size_t)n >= outlen - used)
				break;
			used += (size_t)n;
		}
	}
	return 0;
}

int mesh_is_channel_valid(const char *band, const char *channel)
{
	int ch, start, end;
	char valid[1024];
	char needle[16];

	if (!band || !channel)
		return 1;
	ch = atoi(channel);
	if (!strcmp(band, "2g")) {
		start = 1;
		end = 13;
	} else if (!strcmp(band, "5g")) {
		start = 36;
		end = 177;
	} else if (!strcmp(band, "6g")) {
		start = 1;
		end = 233;
	} else if (!strcmp(band, "60g")) {
		start = 1;
		end = 8;
	} else {
		return 1;
	}
	if (ch < start || ch > end)
		return 1;
	if (mesh_get_valid_channels(valid, sizeof(valid)) != 0)
		return 1;
	snprintf(needle, sizeof(needle), "%d", ch);
	{
		char *p = valid, *tok;
		while ((tok = strsep(&p, " ")) != NULL) {
			if (!strcmp(tok, needle))
				return 0;
		}
	}
	return 1;
}

int mesh_refresh_bridgemac(struct mesh_ctx *ctx)
{
	char device[64];
	char status[16];
	char out[1024];
	char content[256];
	char *argv[] = { "ip", "link", NULL };

	if (mesh_uci_get("network.lan.device", device, sizeof(device)) != 0 || !device[0]) {
		char key[96];
		snprintf(key, sizeof(key), "network.%s.device", ctx->auto_mesh_network);
		if (mesh_uci_get(key, device, sizeof(device)) != 0 || !device[0])
			snprintf(device, sizeof(device), "br-lan");
	}
	snprintf(ctx->device, sizeof(ctx->device), "%s", device);
	mesh_wait_for_interface(ctx, device, status, sizeof(status));
	ctx->devicemac[0] = '\0';
	if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
		char *p = out, *line;
		int take = 0;
		while ((line = strsep(&p, "\n")) != NULL) {
			if (strstr(line, device) && strstr(line, ":"))
				take = 1;
			else if (take && strstr(line, "link/ether")) {
				char *m = strstr(line, "link/ether ");
				if (m) {
					sscanf(m + 11, "%31s", ctx->devicemac);
					break;
				}
			} else if (take && line[0] && line[0] != ' ') {
				take = 0;
			}
		}
	}
	snprintf(content, sizeof(content),
		 "device=\"%s\" ; devicemac=\"%s\"\n", ctx->device, ctx->devicemac);
	mesh_file_write(MESH11SD_TMPDIR "/devicemac", content);
	return ctx->devicemac[0] ? 0 : -1;
}

int mesh_sys_setup(struct mesh_ctx *ctx)
{
	const char *dev = ctx->device[0] ? ctx->device : "br-lan";
	char path[192];
	FILE *f;

	mesh_refresh_bridgemac(ctx);
	dev = ctx->device[0] ? ctx->device : "br-lan";

#define WRITE_SYS(fmt, val) do { \
	snprintf(path, sizeof(path), fmt, val); \
	f = fopen(path, "w"); \
	if (f) { fputs("1\n", f); fclose(f); } \
} while (0)

	/* arp/ndp defaults matching v7 sys_setup (simplified critical set) */
	{
		const char *keys[] = {
			"/proc/sys/net/ipv4/conf/all/arp_accept",
			"/proc/sys/net/ipv4/conf/default/arp_accept",
			NULL
		};
		int i;
		for (i = 0; keys[i]; i++) {
			f = fopen(keys[i], "w");
			if (f) { fputs("1\n", f); fclose(f); }
		}
		snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/%s/arp_accept", dev);
		f = fopen(path, "w");
		if (f) { fputs("1\n", f); fclose(f); }

		f = fopen("/proc/sys/net/ipv6/conf/all/forwarding", "w");
		if (f) { fputs("1\n", f); fclose(f); }
		f = fopen("/proc/sys/net/ipv6/conf/default/forwarding", "w");
		if (f) { fputs("1\n", f); fclose(f); }
		snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/forwarding", dev);
		f = fopen(path, "w");
		if (f) { fputs("1\n", f); fclose(f); }
		snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/accept_ra", dev);
		f = fopen(path, "w");
		if (f) { fputs("0\n", f); fclose(f); }
		f = fopen("/proc/sys/net/ipv6/conf/all/accept_ra", "w");
		if (f) { fputs("2\n", f); fclose(f); }
		f = fopen("/proc/sys/net/ipv6/conf/default/accept_ra", "w");
		if (f) { fputs("2\n", f); fclose(f); }
	}
#undef WRITE_SYS
	return 0;
}

int mesh_set_txpower_uci(struct mesh_ctx *ctx)
{
	char show[4096];
	char *p, *line;

	if (!ctx->txpower[0])
		return 0;
	if (mesh_uci_show_filter("wireless", "wifi-device", show, sizeof(show)) != 0)
		return -1;
	p = show;
	while ((line = strsep(&p, " \n")) != NULL) {
		char *eq = strchr(line, '=');
		char radio[64];
		char cur[32];
		char setline[128];
		if (!eq)
			continue;
		*eq = '\0';
		snprintf(radio, sizeof(radio), "%s", line);
		snprintf(cur, sizeof(cur), "%s.txpower", radio);
		if (mesh_uci_get(cur, cur, sizeof(cur)) != 0) {
			snprintf(setline, sizeof(setline), "set %s.txpower='%s'",
				 radio, ctx->txpower);
			mesh_uci_batch_line(setline);
		}
	}
	return 0;
}

int mesh_cli_txpower(struct mesh_ctx *ctx, const char *arg)
{
	char status[16];
	int i, current = 0, new_power;
	char out[256];

	mesh_get_mesh_iflist(ctx);
	printf("===========================================================================\n");
	printf("Change the mesh transmit power\n");
	if (ctx->iflist_count == 0) {
		printf("    No mesh interfaces found\n");
		printf("===========================================================================\n\n");
		return 0;
	}
	for (i = 0; i < ctx->iflist_count; i++) {
		mesh_wait_for_interface(ctx, ctx->iflist[i], status, sizeof(status));
		if (!strcmp(status, "down"))
			continue;
		{
			char *argv[] = { "iw", "dev", ctx->iflist[i], "info", NULL };
			if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
				char *tp = strstr(out, "txpower");
				if (tp)
					sscanf(tp, "txpower %d", &current);
			}
		}
		break;
	}
	if (!arg || !arg[0]) {
		printf("    Current TX power [ %d ] dBm \n", current);
		printf("    Usage: mesh11sd txpower [+|-] \n");
		printf("    where \"+\" increments by 3dBm and \"-\" decrements by 3dBm\n");
	} else if (!strcmp(arg, "+") || !strcmp(arg, "-")) {
		printf("    Previous TX power [ %d ] dBm \n", current);
		new_power = !strcmp(arg, "+") ? current + 3 : current - 3;
		{
			char pwr[16];
			char *argv[] = { "iw", "dev", ctx->iflist[i], "set", "txpower",
					 "fixed", pwr, NULL };
			snprintf(pwr, sizeof(pwr), "%d", new_power * 100);
			mesh_run_capture(argv, NULL, 0);
		}
		{
			char *argv[] = { "iw", "dev", ctx->iflist[i], "info", NULL };
			if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
				char *tp = strstr(out, "txpower");
				if (tp)
					sscanf(tp, "txpower %d", &current);
			}
		}
		printf("\n    New TX power - [ %d ] dBm \n", current);
		snprintf(ctx->txpower, sizeof(ctx->txpower), "%d", current);
		mesh_uci_set("mesh11sd.setup.txpower", ctx->txpower);
	}
	printf("===========================================================================\n\n");
	return 0;
}

int mesh_cli_stations(struct mesh_ctx *ctx)
{
	char status[16];
	int i;

	mesh_get_mesh_iflist(ctx);
	printf("===========================================================================\n");
	for (i = 0; i < ctx->iflist_count; i++) {
		mesh_wait_for_interface(ctx, ctx->iflist[i], status, sizeof(status));
		if (!strcmp(status, "down"))
			continue;
		{
			char *argv[] = { "iw", "dev", ctx->iflist[i], "station", "dump", NULL };
			char out[16384];
			if (mesh_run_capture(argv, out, sizeof(out)) == 0)
				fputs(out, stdout);
			if (out[0] && out[strlen(out) - 1] != '\n')
				fputc('\n', stdout);
		}
	}
	printf("===========================================================================\n\n");
	return 0;
}

int mesh_cli_country(struct mesh_ctx *ctx, const char *code)
{
	char out[1024];
	char *argv[] = { "iw", "reg", "get", NULL };

	if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
		char *p = strstr(out, "country");
		if (p) {
			char line[128];
			sscanf(p, "%127[^\n]", line);
			printf("%s\n", line);
		} else {
			fputs(out, stdout);
		}
	}
	if (!code || !code[0])
		return 0;
	if (ctx->commit_all) {
		printf("Unable to change regulatory domain. Auto config is committed - edit wireless config to change country settings.\n");
		return 1;
	}
	{
		char line[128];
		snprintf(line, sizeof(line), "set mesh11sd.setup.country='%s'", code);
		/* commit_single_change semantics */
		{
			char changes[4096];
			mesh_uci_changes("mesh11sd", changes, sizeof(changes));
			mesh_uci_revert("mesh11sd");
			mesh_uci_batch_line(line);
			mesh_uci_commit("mesh11sd");
			/* restore other uncommitted - simplified: re-apply changes lines */
			{
				char *p = changes, *ln;
				while ((ln = strsep(&p, "\n")) != NULL) {
					char setl[512];
					if (!ln[0] || strstr(ln, "country"))
						continue;
					snprintf(setl, sizeof(setl), "set %s", ln);
					mesh_uci_batch_line(setl);
				}
			}
		}
		snprintf(ctx->country, sizeof(ctx->country), "%s", code);
		printf("Country code set to [ %s ]. Restart or reboot for wireless to apply.\n", code);
	}
	return 0;
}

int mesh_cli_mesh_rssi_threshold(struct mesh_ctx *ctx, const char *dir, int force)
{
	char status[16];
	int i, current = 0, newv;
	char out[128];

	(void)force;
	mesh_get_mesh_iflist(ctx);
	printf("===========================================================================\n");
	for (i = 0; i < ctx->iflist_count; i++) {
		mesh_wait_for_interface(ctx, ctx->iflist[i], status, sizeof(status));
		if (!strcmp(status, "down"))
			continue;
		{
			char *argv[] = { "iw", "dev", ctx->iflist[i], "get", "mesh_param",
					 "mesh_rssi_threshold", NULL };
			if (mesh_run_capture(argv, out, sizeof(out)) == 0)
				current = atoi(out);
		}
		break;
	}
	if (!dir || !dir[0]) {
		printf("    Current mesh_rssi_threshold [ %d ]\n", current);
		printf("    Usage: mesh11sd mesh_rssi_threshold [+|-] [force]\n");
	} else if (!strcmp(dir, "+") || !strcmp(dir, "-")) {
		newv = !strcmp(dir, "+") ? current + 3 : current - 3;
		{
			char val[16];
			char *argv[] = { "iw", "dev", ctx->iflist[i], "set", "mesh_param",
					 "mesh_rssi_threshold", val, NULL };
			snprintf(val, sizeof(val), "%d", newv);
			mesh_run_capture(argv, NULL, 0);
			mesh_uci_set("mesh11sd.mesh_params.mesh_rssi_threshold", val);
			printf("    New mesh_rssi_threshold [ %d ]\n", newv);
		}
	}
	printf("===========================================================================\n\n");
	return 0;
}
