/* SPDX-License-Identifier: GPL-2.0-or-later
 * Portal ULA metric/hysteresis + MBP/CPE L3 upstream detect (v7-faithful).
 */
#include "mesh11sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Probe a portal ULA with three pings a second apart, then wait up to
 * 3 s for a reply. Any one reply counts, so a slow or lossy mesh path
 * still passes, while a dead portal costs about 5 s rather than 20 s
 * (v7 used a single ping with a 20 s wait). Returns 0 on a reply. */
int mesh_portal_probe(const char *addr)
{
	char *argv[] = { "ping", "-c", "3", "-W", "3", (char *)addr, NULL };
	return mesh_run_capture(argv, NULL, 0) == 0 ? 0 : 1;
}

static int ping_ok(struct mesh_ctx *ctx, const char *addr)
{
	(void)ctx;
	return mesh_portal_probe(addr) == 0;
}


int mesh_detect_l3_upstream(struct mesh_ctx *ctx)
{
	char out[4096];
	char *show[] = { "ip", "link", "show", ctx->up_zone, NULL };
	char *r6[] = { "ip", "-6", "route", "show", "default", "dev", ctx->up_zone, NULL };
	char *r4[] = { "ip", "-4", "route", "show", "default", "dev", ctx->up_zone, NULL };
	char *p;

	ctx->default_ipv6_gw[0] = '\0';
	ctx->default_ipv4_gw[0] = '\0';
	if (!ctx->up_zone[0])
		return 1;
	if (mesh_run_capture(show, out, sizeof(out)) != 0)
		return 1;
	if (!strstr(out, "state UP") && !strstr(out, "state UNKNOWN"))
		return 1;

	if (mesh_run_capture(r6, out, sizeof(out)) == 0) {
		/* Prefer fe80 via; skip "from" lines — simplified v7 awk */
		char *line, *save = out;
		while ((line = strsep(&save, "\n")) != NULL) {
			if (strstr(line, " from "))
				continue;
			p = strstr(line, "via ");
			if (!p)
				continue;
			p += 4;
			sscanf(p, "%63s", ctx->default_ipv6_gw);
			if (!strncmp(ctx->default_ipv6_gw, "fe80:", 5))
				break;
		}
		if (!ctx->default_ipv6_gw[0]) {
			save = out;
			/* retry any via */
			/* out may be consumed; re-fetch */
			mesh_run_capture(r6, out, sizeof(out));
			save = out;
			while ((line = strsep(&save, "\n")) != NULL) {
				if (strstr(line, " from "))
					continue;
				p = strstr(line, "via ");
				if (!p)
					continue;
				sscanf(p + 4, "%63s", ctx->default_ipv6_gw);
				if (ctx->default_ipv6_gw[0])
					break;
			}
		}
	}
	if (mesh_run_capture(r4, out, sizeof(out)) == 0) {
		char *line, *save = out;
		while ((line = strsep(&save, "\n")) != NULL) {
			p = strstr(line, "via ");
			if (!p)
				continue;
			sscanf(p + 4, "%63s", ctx->default_ipv4_gw);
			if (ctx->default_ipv4_gw[0])
				break;
		}
	}
	return (ctx->default_ipv6_gw[0] || ctx->default_ipv4_gw[0]) ? 0 : 1;
}

int mesh_detect_mbp_upstream(struct mesh_ctx *ctx)
{
	char zone[64] = "";

	if (mesh_uci_get("network.lan.device", zone, sizeof(zone)) != 0 || !zone[0])
		snprintf(zone, sizeof(zone), "br-%s", ctx->auto_mesh_network);
	snprintf(ctx->up_zone, sizeof(ctx->up_zone), "%s", zone);
	return mesh_detect_l3_upstream(ctx);
}

/* v7 detect_cpe_upstream: a CPE's upstream is the mesh, on br-wan */
int mesh_detect_cpe_upstream(struct mesh_ctx *ctx)
{
	char zone[64] = "";

	if (mesh_uci_get("network.wan.device", zone, sizeof(zone)) != 0 || !zone[0])
		snprintf(zone, sizeof(zone), "br-wan");
	snprintf(ctx->up_zone, sizeof(ctx->up_zone), "%s", zone);
	return mesh_detect_l3_upstream(ctx);
}

int mesh_update_known_portals_from_ras(struct mesh_ctx *ctx)
{
	char out[4096];
	char tmp[128], known[128];
	char mac_clean[32] = "";
	char eui[40];
	char node_id[8];
	FILE *tf;
	char *argv[] = { "ip", "-6", "addr", "show", "dev", ctx->device, "scope", "global", NULL };
	const char *p;
	int i, n;

	snprintf(tmp, sizeof(tmp), "%s/known_portal_ulas.tmp", MESH11SD_TMPDIR);
	snprintf(known, sizeof(known), "%s/known_portal_ulas", MESH11SD_TMPDIR);
	unlink(tmp);
	mesh_file_write(tmp, "");

	mesh_refresh_bridgemac(ctx);
	mesh_set_ula_prefix(ctx, "get");
	if (!ctx->device[0] || !ctx->ula_base[0])
		return 0;

	/* EUI-64 insert fffe from bridge MAC; match last hextet as node_id */
	for (i = 0, n = 0; ctx->devicemac[i] && n < 12; i++) {
		if (ctx->devicemac[i] != ':')
			mac_clean[n++] = ctx->devicemac[i];
	}
	mac_clean[n] = '\0';
	if (n < 12)
		return 0;
	snprintf(eui, sizeof(eui), "%.6sfffe%.6s", mac_clean, mac_clean + 6);
	snprintf(node_id, sizeof(node_id), "%.4s", eui + 12);

	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return 0;
	tf = fopen(tmp, "a");
	p = out;
	while (p && *p) {
		char line[256];
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';
		p = nl ? nl + 1 : NULL;
		if (!strstr(line, ctx->ula_base) || !strstr(line, node_id))
			continue;
		{
			char addr[96] = "";
			char *tok = strstr(line, "inet6 ");
			if (!tok)
				continue;
			sscanf(tok + 6, "%95s", addr);
			/* strip /prefix */
			{
				char *sl = strchr(addr, '/');
				if (sl)
					*sl = '\0';
			}
			/* portal candidate = first 4 hextets ::1 */
			{
				unsigned h1, h2, h3, h4;
				char cand[80];
				if (sscanf(addr, "%x:%x:%x:%x", &h1, &h2, &h3, &h4) >= 4) {
					snprintf(cand, sizeof(cand), "%x:%x:%x:%x::1",
						 h1, h2, h3, h4);
					if (tf)
						fprintf(tf, "%s\n", cand);
					mesh_log_debug(ctx, "Detected portal candidate [ %s ]", cand);
				}
			}
		}
	}
	if (tf)
		fclose(tf);
	{
		FILE *kf = fopen(tmp, "r");
		int count = 0;
		char line[128];
		if (kf) {
			while (fgets(line, sizeof(line), kf))
				if (line[0] && line[0] != '\n')
					count++;
			fclose(kf);
		}
		if (count > 0) {
			rename(tmp, known);
			mesh_log_debug(ctx, "Discovered %d remote portal ULA(s)", count);
		}
	}
	return 0;
}

int mesh_calc_airtime_metric(struct mesh_ctx *ctx, const char *peer_mac,
			     int *metric_out, int *hops_out)
{
	char out[8192];
	char *mpath[] = { "iw", "dev", ctx->active_mesh_ifname, "mpath", "dump", NULL };
	char nexthop[32] = "";
	int metric = 0, hops = 1;
	char *line, *save;
	int single_hop = 1;
	char *sta_argv[8];
	char sta_out[4096];
	int tx_mbit = 0, rx_mbit = 0, tx_pkts = 0, tx_fail = 0, use_mbit;
	int fail_pct, rate, err, denom, tx_time, retx;

	if (!peer_mac || !peer_mac[0] || !ctx->active_mesh_ifname[0])
		return 1;
	if (mesh_run_capture(mpath, out, sizeof(out)) != 0)
		return 1;
	save = out;
	while ((line = strsep(&save, "\n")) != NULL) {
		char dest[32], nh[32];
		int m = 0, h = 1;
		/* DEST NEXT_HOP ... METRIC ... HOP_COUNT PATH_CHANGE — columns vary; v7 uses $1,$2,$5,$11,$12 */
		if (sscanf(line, "%31s %31s", dest, nh) < 2)
			continue;
		if (strcmp(dest, peer_mac) != 0)
			continue;
		{
			/* tokenize for metric/hops loosely */
			char *toks[20];
			int nt = 0;
			char buf[512];
			char *bp, *t;
			snprintf(buf, sizeof(buf), "%s", line);
			bp = buf;
			while (nt < 20 && (t = strsep(&bp, " \t")) != NULL) {
				if (*t)
					toks[nt++] = t;
			}
			if (nt > 4)
				m = atoi(toks[4]);
			if (nt > 10)
				h = atoi(toks[10]);
			snprintf(nexthop, sizeof(nexthop), "%s", nh);
			metric = m;
			hops = h > 0 ? h : 1;
			break;
		}
	}
	if (!nexthop[0]) {
		mesh_log_debug(ctx, "node [ %s ] not in mpath", peer_mac);
		return 1;
	}

	sta_argv[0] = "iw";
	sta_argv[1] = "dev";
	sta_argv[2] = ctx->active_mesh_ifname;
	sta_argv[3] = "station";
	sta_argv[4] = "get";
	sta_argv[5] = (char *)peer_mac;
	sta_argv[6] = NULL;
	if (mesh_run_capture(sta_argv, sta_out, sizeof(sta_out)) != 0 ||
	    !strstr(sta_out, "mesh plink:") || !strstr(sta_out, "ESTAB")) {
		single_hop = 0;
		sta_argv[5] = nexthop;
		mesh_run_capture(sta_argv, sta_out, sizeof(sta_out));
	} else {
		/* check rssi still above threshold */
		char *sigp = strstr(sta_out, "signal avg:");
		int sig = 0;
		char thr[16] = "";
		if (sigp)
			sscanf(sigp + 11, "%d", &sig);
		mesh_uci_get("mesh11sd.mesh_params.mesh_rssi_threshold", thr, sizeof(thr));
		if (thr[0] && sig <= atoi(thr))
			single_hop = 0;
		(void)single_hop;
	}
	{
		char *p = strstr(sta_out, "tx bitrate:");
		if (p)
			sscanf(p + 11, "%d", &tx_mbit);
		p = strstr(sta_out, "rx bitrate:");
		if (p)
			sscanf(p + 11, "%d", &rx_mbit);
		p = strstr(sta_out, "tx packets:");
		if (p)
			sscanf(p + 11, "%d", &tx_pkts);
		p = strstr(sta_out, "tx failed:");
		if (p)
			sscanf(p + 10, "%d", &tx_fail);
	}
	use_mbit = 0;
	if (tx_mbit >= 24)
		use_mbit = tx_mbit;
	else if (rx_mbit >= 24)
		use_mbit = rx_mbit;
	if (!use_mbit)
		use_mbit = 20;
	fail_pct = 0;
	if (tx_pkts > 20 && tx_fail > 0)
		fail_pct = tx_fail * 100 / tx_pkts;
	if (fail_pct > 90)
		fail_pct = 90;
	rate = use_mbit * 10;
	if (rate <= 0)
		return 1;
	err = fail_pct * 256 / 100;
	denom = 256 - err;
	if (denom <= 0)
		return 1;
	tx_time = 256 + 10 * 2097152 / rate;
	retx = 65536 / denom;
	/* v7: nexthop_metric = tx_time*retx/65536; calc_metric = that * hops^1.6 (awk) */
	{
		long base = tx_time * retx / 65536;
		long scaled = base;
		char cbuf[32], hbuf[16], awkout[32];
		char *aargv[] = {
			"awk", "-v", "c", cbuf, "-v", "h", hbuf,
			"BEGIN { printf \"%.0f\\n\", c * (h ^ 1.6) }",
			NULL
		};
		snprintf(cbuf, sizeof(cbuf), "%ld", base);
		snprintf(hbuf, sizeof(hbuf), "%d", hops > 0 ? hops : 1);
		if (mesh_run_capture(aargv, awkout, sizeof(awkout)) == 0 && awkout[0])
			scaled = strtol(awkout, NULL, 10);
		else if (hops > 1)
			scaled = base * hops; /* fallback */
		if (scaled <= 0)
			return 1;
		if (metric_out)
			*metric_out = (int)scaled;
		mesh_log_debug(ctx,
			       "Mesh airtime metric for peer [ %s ] is [ %ld ], mpath [ %d ], hops [ %d ]",
			       peer_mac, scaled, metric, hops);
	}
	if (hops_out)
		*hops_out = hops;
	return 0;
}

int mesh_get_metrics_from_ula(struct mesh_ctx *ctx, const char *ula,
			      int *metric_out, int *hops_out, char *peer_mac, size_t peer_len)
{
	char out[4096];
	char mesh_mac[32] = "";
	char device_mac[32] = "";
	char *neigh[] = { "ip", "-6", "neigh", NULL };
	char *mpp[] = { "iw", "dev", ctx->active_mesh_ifname, "mpp", "dump", NULL };
	char *line, *save;

	if (!ula || !ula[0] || !ctx->active_mesh_ifname[0])
		return 1;
	if (mesh_run_capture(neigh, out, sizeof(out)) != 0)
		return 1;
	save = out;
	while ((line = strsep(&save, "\n")) != NULL) {
		if (!strstr(line, ula))
			continue;
		{
			char *p = strstr(line, "lladdr ");
			if (p)
				sscanf(p + 7, "%31s", mesh_mac);
		}
		if (mesh_mac[0])
			break;
	}
	if (!mesh_mac[0])
		return 1;
	if (mesh_run_capture(mpp, out, sizeof(out)) != 0)
		return 1;
	save = out;
	while ((line = strsep(&save, "\n")) != NULL) {
		if (!strstr(line, mesh_mac))
			continue;
		sscanf(line, "%*s %31s", device_mac);
		if (device_mac[0])
			break;
	}
	if (!device_mac[0])
		return 1;
	if (peer_mac && peer_len)
		snprintf(peer_mac, peer_len, "%s", device_mac);
	if (mesh_calc_airtime_metric(ctx, device_mac, metric_out, hops_out) != 0)
		return 1;
	mesh_log_debug(ctx, "Mesh airtime metric for [ %s ] [ %s ] is [ %d ], hops [ %d ]",
		       ula, device_mac, metric_out ? *metric_out : 0,
		       hops_out ? *hops_out : 0);
	return 0;
}

/* Replace-style discover path used by mesh_get_portal_ula when mode=discover */
int mesh_portal_ula_discover(struct mesh_ctx *ctx)
{
	char known[128];
	char line[128];
	FILE *f;
	char best_ula[64] = "";
	char best_peer[32] = "";
	int best_metric = 0;
	int best_hops = 0;
	char current_ula[64] = "";
	int current_metric = 0;
	int current_hops = 0;
	int current_reachable = 0;
	char content[256];

	mesh_update_known_portals_from_ras(ctx);

	snprintf(known, sizeof(known), "%s/active_portal", MESH11SD_TMPDIR);
	if (access(known, R_OK) == 0) {
		FILE *af = fopen(known, "r");
		if (af && fgets(content, sizeof(content), af)) {
			char *p = strstr(content, "portal_ula=\"");
			if (p)
				sscanf(p + 12, "%63[^\"]", current_ula);
			p = strstr(content, "metric=");
			if (p)
				current_metric = atoi(p + 7);
			p = strstr(content, "hops=");
			if (p)
				current_hops = atoi(p + 5);
			p = strstr(content, "portal_mac=\"");
			if (p)
				sscanf(p + 12, "%31[^\"]", best_peer);
		}
		if (af)
			fclose(af);
		if (current_ula[0] && ping_ok(ctx, current_ula)) {
			current_reachable = 1;
			snprintf(best_ula, sizeof(best_ula), "%s", current_ula);
			best_metric = current_metric;
			best_hops = current_hops;
		}
	}

	snprintf(known, sizeof(known), "%s/known_portal_ulas", MESH11SD_TMPDIR);
	f = fopen(known, "r");
	if (!f) {
		if (!best_ula[0]) {
			ctx->link_to_portal = 1;
			unlink(MESH11SD_TMPDIR "/active_portal");
			return 1;
		}
	} else {
		while (fgets(line, sizeof(line), f)) {
			char cand[64];
			int metric = 0, hops = 0;
			char peer[32] = "";
			size_t n;
			n = strlen(line);
			while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
				line[--n] = '\0';
			if (!line[0])
				continue;
			snprintf(cand, sizeof(cand), "%s", line);
			{
				if (mesh_portal_probe(cand) != 0)
					continue;
			}
			if (mesh_get_metrics_from_ula(ctx, cand, &metric, &hops, peer, sizeof(peer)) != 0)
				continue;
			if (!current_ula[0] || !current_reachable) {
				snprintf(current_ula, sizeof(current_ula), "%s", cand);
				current_metric = metric;
				current_reachable = 1;
				snprintf(best_ula, sizeof(best_ula), "%s", cand);
				best_metric = metric;
				best_hops = hops;
				snprintf(best_peer, sizeof(best_peer), "%s", peer);
				continue;
			}
			if (best_metric == 0) {
				best_metric = metric;
				snprintf(best_ula, sizeof(best_ula), "%s", cand);
				best_hops = hops;
				snprintf(best_peer, sizeof(best_peer), "%s", peer);
			}
			if (!strcmp(cand, current_ula)) {
				current_metric = metric;
				if (!strcmp(best_ula, cand) || metric < best_metric) {
					best_metric = metric;
					snprintf(best_ula, sizeof(best_ula), "%s", cand);
					best_hops = hops;
					snprintf(best_peer, sizeof(best_peer), "%s", peer);
				}
			} else if ((current_metric - metric) >= ctx->mesh_metric_threshold &&
				   metric < best_metric) {
				best_metric = metric;
				snprintf(best_ula, sizeof(best_ula), "%s", cand);
				best_hops = hops;
				snprintf(best_peer, sizeof(best_peer), "%s", peer);
			}
		}
		fclose(f);
	}

	if (!best_ula[0])
		snprintf(best_ula, sizeof(best_ula), "%s", current_ula);
	if (!best_ula[0]) {
		mesh_log_warn(ctx, "No responsive portal found");
		unlink(MESH11SD_TMPDIR "/active_portal");
		ctx->link_to_portal = 1;
		return 1;
	}
	{
		if (mesh_portal_probe(best_ula) != 0) {
			mesh_log_warn(ctx, "No responsive portal found");
			unlink(MESH11SD_TMPDIR "/active_portal");
			ctx->link_to_portal = 1;
			return 1;
		}
	}
	{
		char base[32], subnet[8];

		/* Keep hextet text as-is (v7 awk). %x drops leading zeros and
		 * can rewrite ula_base so later stale-address matches miss. */
		if (mesh_ula_parts_from_addr(best_ula, base, sizeof(base),
					     subnet, sizeof(subnet)) == 0) {
			snprintf(ctx->ula_base, sizeof(ctx->ula_base), "%s", base);
			snprintf(ctx->ula_subnet, sizeof(ctx->ula_subnet), "%s", subnet);
		}
	}
	snprintf(ctx->portal_ula, sizeof(ctx->portal_ula), "%s", best_ula);
	ctx->portal_metric = best_metric;
	ctx->portal_hops = best_hops;
	snprintf(ctx->portal_mac, sizeof(ctx->portal_mac), "%s", best_peer);
	ctx->link_to_portal = 0;
	mesh_log_info(ctx,
		      "Selected portal: [ %s ] [ %s ], mesh metric delta [ %d ], mesh airtime metric [ %d ], hops [ %d ]",
		      best_ula, best_peer, ctx->mesh_metric_threshold, best_metric, best_hops);
	snprintf(content, sizeof(content),
		 "portal_ula=\"%s\"; ula_subnet=\"%s\"; portal_mac=\"%s\"; metric=%d; hops=%d\n",
		 best_ula, ctx->ula_subnet, best_peer, best_metric, best_hops);
	mesh_file_write(MESH11SD_TMPDIR "/active_portal", content);
	return 0;
}

