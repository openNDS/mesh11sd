/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 7: mobility_settings, HWMP rootmode, path cost, channel track, check_gate.
 */
#include "mesh11sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int mesh_set_iface_path_cost(struct mesh_ctx *ctx, int path_cost,
			     const char *bridge, const char *iface)
{
	char path[192];
	char out[2048];
	char *show[] = { "brctl", "showstp", (char *)bridge, NULL };
	int other = 65535 - path_cost;
	int stpstate = 1;
	FILE *f;

	if (!bridge || !bridge[0])
		return 1;
	if (path_cost == 0) {
		snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/stp_state", bridge);
		f = fopen(path, "w");
		if (f) { fputs("0\n", f); fclose(f); }
		return 0;
	}
	if (path_cost <= 0 || path_cost >= 65535)
		return 1;

	snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/stp_state", bridge);
	f = fopen(path, "w");
	if (f) { fprintf(f, "%d\n", stpstate); fclose(f); }

	if (mesh_run_capture(show, out, sizeof(out)) != 0)
		return 1;
	{
		char *p = out, *line;
		while ((line = strsep(&p, "\n")) != NULL) {
			char br_iface[64];
			if (!strstr(line, "("))
				continue;
			if (sscanf(line, "%63s", br_iface) != 1)
				continue;
			{
				char pcpath[192];
				int want = (!iface || !strcmp(br_iface, iface)) ? path_cost : other;
				int cur = -1;
				snprintf(pcpath, sizeof(pcpath),
					 "/sys/class/net/%s/brport/path_cost", br_iface);
				f = fopen(pcpath, "r");
				if (f) {
					if (fscanf(f, "%d", &cur) != 1)
						cur = -1;
					fclose(f);
				}
				if (cur != want) {
					f = fopen(pcpath, "w");
					if (f) {
						fprintf(f, "%d\n", want);
						fclose(f);
					}
				}
			}
		}
	}
	(void)ctx;
	return 0;
}

int mesh_rootmode(struct mesh_ctx *ctx)
{
	char cur[16] = "";

	mesh_log_debug(ctx, "Checking mesh_hwmp_rootmode");
	mesh_uci_get("mesh11sd.mesh_params.mesh_hwmp_rootmode", cur, sizeof(cur));
	if (ctx->portal_detect == 0 || ctx->portal_detect == 4 ||
	    !strcmp(ctx->detected_state, "portal")) {
		if (atoi(cur) != 4)
			mesh_uci_set("mesh11sd.mesh_params.mesh_hwmp_rootmode", "4");
	}
	return 0;
}

int mesh_mobility_settings(struct mesh_ctx *ctx, const char *mode)
{
	int level = ctx->mesh_node_mobility_level;
	int rts, limit, quantum, aql_threshold, beacon_int, dtim_period, rootmode;
	int mesh_fwding = 1;
	int rann, preq_min, path_timeout, max_peers, max_preq;
	const char *rssi;
	char stanza[96] = "", radio[32] = "", phy[32] = "";
	char show[4096], setl[192];
	char content[384];
	int do_config, do_runtime;

	if (level < 0 || level > 6)
		level = 2;

	do_config = (!mode || !strcmp(mode, "config") || !strcmp(mode, "all"));
	do_runtime = (!mode || !strcmp(mode, "runtime") || !strcmp(mode, "parameters") ||
		      !strcmp(mode, "all"));

	/* resolve active mesh radio/phy early (needed for driver/MLO overrides) */
	if (!ctx->active_mesh_ifname[0] &&
	    access(MESH11SD_TMPDIR "/active_mesh_ifname", R_OK) == 0) {
		FILE *f = fopen(MESH11SD_TMPDIR "/active_mesh_ifname", "r");
		char line[128];
		if (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "active_mesh_ifname=\"");
			if (p)
				sscanf(p + 20, "%31[^\"]", ctx->active_mesh_ifname);
		}
		if (f)
			fclose(f);
	}
	if (ctx->active_mesh_ifname[0] &&
	    mesh_uci_show_filter("wireless", ctx->active_mesh_ifname, show, sizeof(show)) == 0) {
		char *p = show, *line;
		while ((line = strsep(&p, "\n")) != NULL) {
			char *eq = strchr(line, '=');
			char *dot;
			if (!eq || !strstr(line, ".ifname="))
				continue;
			dot = strchr(line, '.');
			if (!dot)
				continue;
			{
				char *dot2 = strchr(dot + 1, '.');
				size_t n;
				if (!dot2)
					continue;
				n = (size_t)(dot2 - line);
				if (n >= sizeof(stanza))
					n = sizeof(stanza) - 1;
				memcpy(stanza, line, n);
				stanza[n] = '\0';
			}
			break;
		}
	}
	if (stanza[0]) {
		char key[128];
		snprintf(key, sizeof(key), "%s.device", stanza);
		mesh_uci_get(key, radio, sizeof(radio));
		if (radio[0])
			mesh_radiophy(radio, phy, sizeof(phy));
	}
	if (!radio[0] && ctx->radios[0][0]) {
		snprintf(radio, sizeof(radio), "%s", ctx->radios[0]);
		mesh_radiophy(radio, phy, sizeof(phy));
	}

	/* v7: populate mesh_driver from phy uevent (same source as wifidetect) */
	if (phy[0]) {
		char uevent[160], dline[128];
		FILE *df;
		snprintf(uevent, sizeof(uevent),
			 "/sys/class/ieee80211/%s/device/uevent", phy);
		df = fopen(uevent, "r");
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

	switch (level) {
	case 0:
		/* Static – kernel/driver defaults: delete mobility UCI overrides */
		if (do_config) {
			if (radio[0]) {
				snprintf(setl, sizeof(setl), "del wireless.%s.beacon_int", radio);
				mesh_uci_batch_line(setl);
			}
			mesh_uci_batch_line("del mesh11sd.mesh_params.mesh_fwding");
			mesh_uci_batch_line("del mesh11sd.mesh_params.mesh_hwmp_rann_interval");
			mesh_uci_batch_line("del mesh11sd.mesh_params.mesh_hwmp_preq_min_interval");
			mesh_uci_batch_line("del mesh11sd.mesh_params.mesh_hwmp_active_path_timeout");
			mesh_uci_batch_line("del mesh11sd.mesh_params.mesh_hwmp_max_preq_retries");
			mesh_uci_batch_line("del mesh11sd.mesh_params.mesh_hwmp_rootmode");
		}
		snprintf(content, sizeof(content),
			 "mesh_node_mobility_level=\"0\"; rootmode=\"\"; beacon_int=\"\"; "
			 "dtim_period=\"\"; rts=\"\"; limit=\"\"; quantum=\"\"; aql_threshold=\"\"\n");
		mesh_file_write(MESH11SD_TMPDIR "/mesh_mobility", content);
		mesh_log_debug(ctx, "mobility_settings level=0 (defaults)");
		return 0;
	case 1:
		rts = 500; limit = 1000; quantum = 3000; aql_threshold = 2500;
		beacon_int = 50; dtim_period = 2; rootmode = 3;
		rann = 2000; preq_min = 500; path_timeout = 1500;
		max_peers = 16; rssi = "-68"; max_preq = 4;
		break;
	case 2:
		rts = 300; limit = 800; quantum = 4500; aql_threshold = 1800;
		beacon_int = 25; dtim_period = 2; rootmode = 3;
		rann = 1500; preq_min = 400; path_timeout = 1100;
		max_peers = 32; rssi = "-72"; max_preq = 3;
		break;
	case 3:
		rts = 200; limit = 500; quantum = 6000; aql_threshold = 1200;
		beacon_int = 20; dtim_period = 1; rootmode = 2;
		rann = 1000; preq_min = 350; path_timeout = 700;
		max_peers = 64; rssi = "-75"; max_preq = 2;
		break;
	case 4:
		rts = 120; limit = 300; quantum = 8000; aql_threshold = 700;
		beacon_int = 15; dtim_period = 1; rootmode = 2;
		rann = 500; preq_min = 300; path_timeout = 450;
		max_peers = 96; rssi = "-78"; max_preq = 1;
		break;
	case 5:
		rts = 80; limit = 150; quantum = 9000; aql_threshold = 400;
		beacon_int = 15; dtim_period = 1; rootmode = 0;
		rann = 300; preq_min = 250; path_timeout = 400;
		max_peers = 96; rssi = "-81"; max_preq = 1;
		break;
	case 6:
		rts = 60; limit = 100; quantum = 7000; aql_threshold = 300;
		beacon_int = 15; dtim_period = 1; rootmode = 0;
		rann = 250; preq_min = 200; path_timeout = 350;
		max_peers = 64; rssi = "-79"; max_preq = 1;
		break;
	default:
		return 0;
	}

	if (!strcmp(ctx->detected_state, "portal") || ctx->portal_detect == 0 ||
	    ctx->portal_detect == 4)
		rootmode = 4;

	/* v7 ~4369: use_default_beacon_interval forces 100 */
	if (ctx->use_default_beacon_interval > 0) {
		beacon_int = 100;
		mesh_log_info(ctx,
			      "Default beacon interval requested [ %d ] Mesh mobility may be suboptimal.",
			      beacon_int);
	}

	/* v7 ~4388/4397: known drivers that reject short beacon intervals */
	if (!strcmp(ctx->mesh_driver, "ath11k")) {
		beacon_int = 100;
		mesh_log_info(ctx,
			      "Qualcomm ath11k detected, beacon interval override to [ %d ms ] (workaround for ath11k bug). Mesh mobility may be suboptimal.",
			      beacon_int);
	}
	if (!strcmp(ctx->mesh_driver, "mt76_wmac")) {
		beacon_int = 100;
		mesh_log_info(ctx,
			      "MediaTek mt76_wmac detected, beacon interval override to [ %d ms ] (workaround for mt7603 beacon bug). Mesh mobility may be suboptimal.",
			      beacon_int);
	}

	/* v7 ~4405: multi-band wiphy (MLO) — stream iw phy info (can be >32KB) */
	if (phy[0] && mesh_phy_is_multi_band(phy)) {
		beacon_int = 100;
		mesh_log_info(ctx,
			      "Multi band wiphy detected, beacon interval override to [ %d ms ] (workaround for MLO bug). Mesh mobility may be suboptimal.",
			      beacon_int);
	}

	ctx->mesh_beacon_interval = beacon_int;
	ctx->mesh_dtim_period = dtim_period;

	if (phy[0] && do_runtime) {
		char rts_s[16], lim_s[16], q_s[16], aql_s[16];
		char *a1[] = { "iw", "phy", phy, "set", "rts", rts_s, NULL };
		char *a2[] = { "iw", "phy", phy, "set", "txq", "limit", lim_s, NULL };
		char *a3[] = { "iw", "phy", phy, "set", "txq", "quantum", q_s, NULL };
		char aqlpath[160];
		snprintf(rts_s, sizeof(rts_s), "%d", rts);
		snprintf(lim_s, sizeof(lim_s), "%d", limit);
		snprintf(q_s, sizeof(q_s), "%d", quantum);
		mesh_run_capture(a1, NULL, 0);
		mesh_run_capture(a2, NULL, 0);
		mesh_run_capture(a3, NULL, 0);
		snprintf(aqlpath, sizeof(aqlpath),
			 "/sys/kernel/debug/ieee80211/%s/aql_enable", phy);
		{
			FILE *f = fopen(aqlpath, "w");
			if (f) { fputs("1\n", f); fclose(f); }
		}
		snprintf(aqlpath, sizeof(aqlpath),
			 "/sys/kernel/debug/ieee80211/%s/aql_threshold", phy);
		{
			FILE *f = fopen(aqlpath, "w");
			if (f) {
				snprintf(aql_s, sizeof(aql_s), "%d\n", aql_threshold);
				fputs(aql_s, f);
				fclose(f);
			}
		}
	}

	if (do_config) {
		if (radio[0]) {
			/* v7: del then set so a prior sticky value cannot linger */
			snprintf(setl, sizeof(setl), "del wireless.%s.beacon_int", radio);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.%s.beacon_int='%d'",
				 radio, beacon_int);
			mesh_uci_batch_line(setl);
		}
		if (stanza[0]) {
			snprintf(setl, sizeof(setl), "set %s.dtim_period='%d'",
				 stanza, dtim_period);
			mesh_uci_batch_line(setl);
		}
		snprintf(setl, sizeof(setl), "set mesh11sd.mesh_params.mesh_fwding='%d'",
			 mesh_fwding);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_hwmp_rann_interval='%d'", rann);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_hwmp_preq_min_interval='%d'", preq_min);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_hwmp_active_path_timeout='%d'",
			 path_timeout);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_max_peer_links='%d'", max_peers);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_rssi_threshold='%s'", rssi);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_hwmp_max_preq_retries='%d'", max_preq);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_hwmp_rootmode='%d'", rootmode);
		mesh_uci_batch_line(setl);
	}

	{
		char br[64];
		snprintf(br, sizeof(br), "br-%s", ctx->auto_mesh_network);
		mesh_set_iface_path_cost(ctx, ctx->mesh_path_cost, br,
					 ctx->active_mesh_ifname[0] ? ctx->active_mesh_ifname : NULL);
	}

	snprintf(content, sizeof(content),
		 "mesh_node_mobility_level=\"%d\"; rootmode=\"%d\"; beacon_int=\"%d\"; "
		 "dtim_period=\"%d\"; rts=\"%d\"; limit=\"%d\"; quantum=\"%d\"; "
		 "aql_threshold=\"%d\"\n",
		 level, rootmode, beacon_int, dtim_period, rts, limit, quantum,
		 aql_threshold);
	mesh_file_write(MESH11SD_TMPDIR "/mesh_mobility", content);
	mesh_log_debug(ctx, "mobility_settings level=%d rootmode=%d beacon=%d driver=%s",
		       level, rootmode, beacon_int,
		       ctx->mesh_driver[0] ? ctx->mesh_driver : "-");
	return 0;
}


int mesh_check_path_changes(struct mesh_ctx *ctx)
{
	int i;
	char out[8192];

	mesh_get_mesh_iflist(ctx);
	for (i = 0; i < ctx->iflist_count; i++) {
		char status[16];
		char *argv[] = { "iw", "dev", ctx->iflist[i], "mpath", "dump", NULL };
		mesh_wait_for_interface(ctx, ctx->iflist[i], status, sizeof(status));
		if (!strcmp(status, "down"))
			continue;
		if (mesh_run_capture(argv, out, sizeof(out)) != 0)
			continue;
		/* v7: peermac_id state in tmpdir; log multi-hop path changes */
		{
			char *p = out, *line;
			int hdr = 1;
			while ((line = strsep(&p, "\n")) != NULL) {
				char dest[32], nh[32];
				int metric = 0, hops = 0, pchg = 0, seq = 0;
				int prev_pchg = 1, prev_seq = 0;
				char *toks[16];
				int nt = 0;
				char buf[512], *bp, *tok;
				char peermac_id[72];
				char statepath[192];
				char state[256];
				FILE *sf;
				if (hdr) { hdr = 0; continue; }
				snprintf(buf, sizeof(buf), "%s", line);
				bp = buf;
				while (nt < 16 && (tok = strsep(&bp, " \t")) != NULL)
					if (*tok)
						toks[nt++] = tok;
				if (nt < 11)
					continue;
				snprintf(dest, sizeof(dest), "%s", toks[0]);
				snprintf(nh, sizeof(nh), "%s", toks[1]);
				metric = atoi(toks[4]);
				seq = atoi(toks[3]);
				hops = atoi(toks[10]);
				if (nt > 11)
					pchg = atoi(toks[11]);
				if (mesh_sha256_hex(dest, peermac_id, sizeof(peermac_id)) != 0)
					continue;
				snprintf(statepath, sizeof(statepath), "%s/%s",
					 MESH11SD_TMPDIR, peermac_id);
				if ((sf = fopen(statepath, "r"))) {
					if (fgets(state, sizeof(state), sf)) {
						char *q = strstr(state, "previous_path_change_count=");
						if (q)
							prev_pchg = atoi(q + 27);
						q = strstr(state, "previous_sequence_number=");
						if (q)
							prev_seq = atoi(q + 25);
					}
					fclose(sf);
				}
				if (pchg != prev_pchg && hops > 1 && pchg > 1) {
					mesh_log_debug(ctx,
						"Station [ %s ] is [ %d ] hops away, path_change_count [ %d ]",
						dest, hops, pchg);
					mesh_log_info(ctx,
						"Path to [ %s ], changed, [ %d ] hop(s) via [ %s ]",
						dest, hops, nh);
					(void)metric;
					(void)seq;
					(void)prev_seq;
				}
				{
					char content[192];
					snprintf(content, sizeof(content),
						 "previous_path_change_count=%d\n"
						 "previous_sequence_number=%d\n",
						 pchg, seq);
					mesh_file_write(statepath, content);
				}
			}
		}
	}
	return 0;
}

int mesh_check_gate(struct mesh_ctx *ctx)
{
	char out[4096];
	char *argv[] = { "iw", "dev", NULL };

	if (mesh_run_capture(argv, out, sizeof(out)) != 0) {
		mesh_uci_set("mesh11sd.mesh_params.mesh_connected_to_gate", "0");
		return 0;
	}
	if (strstr(out, "type AP"))
		mesh_uci_set("mesh11sd.mesh_params.mesh_connected_to_gate", "1");
	else
		mesh_uci_set("mesh11sd.mesh_params.mesh_connected_to_gate", "0");
	(void)ctx;
	return 0;
}

int mesh_scan_channel(struct mesh_ctx *ctx)
{
	char show[4096];
	char *p, *line;

	mesh_log_debug(ctx, "Entering scan channel, link_to_portal [ %d ]",
		       ctx->link_to_portal);
	if (ctx->link_to_portal <= 0)
		return 0;
	if (mesh_uci_show_filter("wireless", "mode='mesh'", show, sizeof(show)) != 0)
		return 0;
	p = show;
	while ((line = strsep(&p, " \n")) != NULL) {
		char sect[96], *eq, ifn[64], key[128];
		char *scan[] = { "iw", "dev", ifn, "scan", NULL };
		eq = strchr(line, '=');
		if (!eq)
			continue;
		snprintf(sect, sizeof(sect), "%s", line);
		sect[eq - line] = '\0';
		snprintf(key, sizeof(key), "%s.disabled", sect);
		if (mesh_uci_get(key, key, sizeof(key)) == 0 && atoi(key) == 1)
			continue;
		snprintf(key, sizeof(key), "%s.ifname", sect);
		if (mesh_uci_get(key, ifn, sizeof(ifn)) != 0)
			continue;
		mesh_log_info(ctx, "scan_channel iw dev %s scan", ifn);
		/* background-ish: run and discard; channel pick is applied by wifi reload in v7 */
		mesh_run_capture(scan, NULL, 0);
		break;
	}
	return 0;
}

int mesh_check_channel(struct mesh_ctx *ctx)
{
	static unsigned long last_check;
	unsigned long now = 0;
	FILE *f;
	char up[64];

	if (ctx->portal_detect == 4)
		return 0;
	mesh_log_debug(ctx, "checking for working channel....");
	f = fopen("/proc/uptime", "r");
	if (f) {
		if (fgets(up, sizeof(up), f))
			now = (unsigned long)strtoul(up, NULL, 10);
		fclose(f);
	}
	if (last_check &&
	    (now - last_check) < (unsigned long)ctx->channel_tracking_checkinterval) {
		mesh_log_debug(ctx, "Time since last channel tracking check [ %lu ], minimum checkinterval [ %d ]",
			       now - last_check, ctx->channel_tracking_checkinterval);
		return 0;
	}
	last_check = now;
	mesh_get_portal_ula(ctx, "get");
	if (ctx->link_to_portal == 0) {
		mesh_log_debug(ctx, "Check Channel - portal detected");
		return 0;
	}
	mesh_log_debug(ctx, "Check Channel - portal not detected - Scanning...");
	mesh_scan_channel(ctx);
	return 0;
}
