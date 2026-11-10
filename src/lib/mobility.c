/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 7: mobility_settings, HWMP rootmode, path cost, channel track, check_gate.
 */
#include "mesh11sd.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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
		int cur = -1;

		snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/stp_state", bridge);
		mesh_sysfs_read_int(path, &cur);
		if (mesh_sysfs_write_int_if_changed(path, 0) == 1)
			mesh_log_info(ctx, "Old value:%s stp_state=%d, Setting new value:stp_state=0",
				      bridge, cur);
		return 0;
	}
	if (path_cost <= 0 || path_cost >= 65535)
		return 1;

	snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/stp_state", bridge);
	{
		int cur = -1;

		mesh_sysfs_read_int(path, &cur);
		if (mesh_sysfs_write_int_if_changed(path, stpstate) == 1)
			mesh_log_info(ctx, "Old value:%s stp_state=%d, Setting new value:stp_state=%d",
				      bridge, cur, stpstate);
	}

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
					if (mesh_sysfs_write_int_if_changed(pcpath, want) == 1)
						mesh_log_info(ctx,
							      "Old value:%s path_cost=%d, Setting new value:path_cost=%d",
							      br_iface, cur, want);
				}
			}
		}
	}
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


#define PHY_VAL_UNKNOWN	(-1)
#define PHY_VAL_OFF	(-2)

struct mobility_eval {
	int level;
	int valid_targets;
	int rts, limit, quantum, aql_threshold;
	int beacon_int, dtim_period, rootmode;
	int mesh_fwding, rann, preq_min, path_timeout, max_peers, max_preq;
	char rssi[8];
	char stanza[96], radio[32], phy[32];
};

static void phy_val_str(int v, char *buf, size_t len)
{
	if (v == PHY_VAL_OFF)
		snprintf(buf, len, "off");
	else if (v == PHY_VAL_UNKNOWN)
		snprintf(buf, len, "unknown");
	else
		snprintf(buf, len, "%d", v);
}

static void rts_unsup_path(const char *phy, char *buf, size_t len)
{
	snprintf(buf, len, MESH11SD_TMPDIR "/rts_unsupported_%s", phy);
}

static int rts_is_unsupported(const char *phy)
{
	char path[192];

	if (!phy || !phy[0])
		return 0;
	rts_unsup_path(phy, path, sizeof(path));
	return access(path, F_OK) == 0;
}

static void rts_mark_unsupported(struct mesh_ctx *ctx, const char *phy,
				const char *why)
{
	char path[192];

	if (!phy || !phy[0] || rts_is_unsupported(phy))
		return;
	rts_unsup_path(phy, path, sizeof(path));
	mesh_file_write(path, "1\n");
	mesh_log_info(ctx,
		      "rts is unsupported on %s (%s); will not write rts or treat it as a difference",
		      phy, why ? why : "driver ignored it");
}

static void phy_read_aqm(const char *phy, int *limit, int *quantum)
{
	char path[192], line[128];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/kernel/debug/ieee80211/%s/aqm", phy);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		int v;

		if (sscanf(line, "%*s fq_limit %d", &v) == 1)
			*limit = v;
		else if (sscanf(line, "%*s fq_quantum %d", &v) == 1)
			*quantum = v;
	}
	fclose(f);
}

static void phy_read_runtime_values(const char *phy, int *rts, int *limit, int *quantum)
{
	char *argv[] = { "iw", "phy", (char *)phy, "info", NULL };
	int pipefd[2];
	pid_t pid;
	FILE *fp;
	char line[1024];
	int status;

	*rts = *limit = *quantum = PHY_VAL_UNKNOWN;
	if (!phy || !phy[0])
		return;
	if (pipe(pipefd) < 0)
		return;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}
	if (pid == 0) {
		int dn;

		close(pipefd[0]);
		dup2(pipefd[1], 1);
		close(pipefd[1]);
		dn = open("/dev/null", O_WRONLY);
		if (dn >= 0) {
			dup2(dn, 2);
			close(dn);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pipefd[1]);
	fp = fdopen(pipefd[0], "r");
	if (!fp) {
		close(pipefd[0]);
		waitpid(pid, &status, 0);
		return;
	}
	while (fgets(line, sizeof(line), fp)) {
		char *p;

		/* Wiphy-level line is "RTS threshold:"; per-band caps use
		 * "RTS Threshold:" and must not overwrite the real value. */
		if (*rts == PHY_VAL_UNKNOWN && (p = strstr(line, "RTS threshold:"))) {
			p += 14;
			while (*p == ' ' || *p == '\t')
				p++;
			if (!strncmp(p, "off", 3))
				*rts = PHY_VAL_OFF;
			else if (*p == '-' || (*p >= '0' && *p <= '9'))
				*rts = atoi(p);
		} else if ((p = strstr(line, "TXQ quantum:"))) {
			*quantum = atoi(p + 12);
		} else if ((p = strstr(line, "TXQ: quantum:"))) {
			*quantum = atoi(p + 13);
		} else if (strstr(line, "TXQ memory limit:") ||
			   strstr(line, "TXQ: memory_limit:")) {
			continue;
		} else if ((p = strstr(line, "TXQ limit:"))) {
			*limit = atoi(p + 10);
		} else if ((p = strstr(line, "TXQ: limit:"))) {
			*limit = atoi(p + 11);
		}
	}
	fclose(fp);
	waitpid(pid, &status, 0);
	/* iw phy info does not print FQ limit/quantum on mt76; those live in aqm */
	phy_read_aqm(phy, limit, quantum);
	/* Some drivers (ath11k) omit the RTS line; debugfs may exist but stay -1 */
	if (*rts == PHY_VAL_UNKNOWN) {
		char rpath[192];
		int rv;

		snprintf(rpath, sizeof(rpath),
			 "/sys/kernel/debug/ieee80211/%s/rts_threshold", phy);
		if (mesh_sysfs_read_int(rpath, &rv) == 0 && rv >= 0)
			*rts = rv;
	}
}

static int mobility_eval(struct mesh_ctx *ctx, struct mobility_eval *e, int log_info)
{
	char show[4096];

	memset(e, 0, sizeof(*e));
	e->level = ctx->mesh_node_mobility_level;
	e->mesh_fwding = 1;
	e->valid_targets = 0;
	if (e->level < 0 || e->level > 6)
		e->level = 2;

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
				if (n >= sizeof(e->stanza))
					n = sizeof(e->stanza) - 1;
				memcpy(e->stanza, line, n);
				e->stanza[n] = '\0';
			}
			break;
		}
	}
	if (e->stanza[0]) {
		char key[128];
		snprintf(key, sizeof(key), "%s.device", e->stanza);
		mesh_uci_get(key, e->radio, sizeof(e->radio));
		if (e->radio[0])
			mesh_radiophy(e->radio, e->phy, sizeof(e->phy));
	}
	if (!e->radio[0] && ctx->radios[0][0]) {
		snprintf(e->radio, sizeof(e->radio), "%s", ctx->radios[0]);
		mesh_radiophy(e->radio, e->phy, sizeof(e->phy));
	}

	if (e->phy[0]) {
		char uevent[160], dline[128];
		FILE *df;
		snprintf(uevent, sizeof(uevent),
			 "/sys/class/ieee80211/%s/device/uevent", e->phy);
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

	if (e->level == 0)
		return 0;

	switch (e->level) {
	case 1:
		e->rts = 500; e->limit = 1000; e->quantum = 3000; e->aql_threshold = 2500;
		e->beacon_int = 50; e->dtim_period = 2; e->rootmode = 3;
		e->rann = 2000; e->preq_min = 500; e->path_timeout = 1500;
		e->max_peers = 16; snprintf(e->rssi, sizeof(e->rssi), "-68"); e->max_preq = 4;
		break;
	case 2:
		e->rts = 300; e->limit = 800; e->quantum = 4500; e->aql_threshold = 1800;
		e->beacon_int = 25; e->dtim_period = 2; e->rootmode = 3;
		e->rann = 1500; e->preq_min = 400; e->path_timeout = 1100;
		e->max_peers = 32; snprintf(e->rssi, sizeof(e->rssi), "-72"); e->max_preq = 3;
		break;
	case 3:
		e->rts = 200; e->limit = 500; e->quantum = 6000; e->aql_threshold = 1200;
		e->beacon_int = 20; e->dtim_period = 1; e->rootmode = 2;
		e->rann = 1000; e->preq_min = 350; e->path_timeout = 700;
		e->max_peers = 64; snprintf(e->rssi, sizeof(e->rssi), "-75"); e->max_preq = 2;
		break;
	case 4:
		e->rts = 120; e->limit = 300; e->quantum = 8000; e->aql_threshold = 700;
		e->beacon_int = 15; e->dtim_period = 1; e->rootmode = 2;
		e->rann = 500; e->preq_min = 300; e->path_timeout = 450;
		e->max_peers = 96; snprintf(e->rssi, sizeof(e->rssi), "-78"); e->max_preq = 1;
		break;
	case 5:
		e->rts = 80; e->limit = 150; e->quantum = 9000; e->aql_threshold = 400;
		e->beacon_int = 15; e->dtim_period = 1; e->rootmode = 0;
		e->rann = 300; e->preq_min = 250; e->path_timeout = 400;
		e->max_peers = 96; snprintf(e->rssi, sizeof(e->rssi), "-81"); e->max_preq = 1;
		break;
	case 6:
		e->rts = 60; e->limit = 100; e->quantum = 7000; e->aql_threshold = 300;
		e->beacon_int = 15; e->dtim_period = 1; e->rootmode = 0;
		e->rann = 250; e->preq_min = 200; e->path_timeout = 350;
		e->max_peers = 64; snprintf(e->rssi, sizeof(e->rssi), "-79"); e->max_preq = 1;
		break;
	default:
		return -1;
	}

	if (!strcmp(ctx->detected_state, "portal") || ctx->portal_detect == 0 ||
	    ctx->portal_detect == 4)
		e->rootmode = 4;

	if (ctx->use_default_beacon_interval > 0) {
		e->beacon_int = 100;
		if (log_info)
			mesh_log_info(ctx,
				      "Default beacon interval requested [ %d ] Mesh mobility may be suboptimal.",
				      e->beacon_int);
	}

	if (!strcmp(ctx->mesh_driver, "ath11k")) {
		e->beacon_int = 100;
		if (log_info)
			mesh_log_info(ctx,
				      "Qualcomm ath11k detected, beacon interval override to [ %d ms ] (workaround for ath11k bug). Mesh mobility may be suboptimal.",
				      e->beacon_int);
	}
	if (!strcmp(ctx->mesh_driver, "mt76_wmac")) {
		e->beacon_int = 100;
		if (log_info)
			mesh_log_info(ctx,
				      "MediaTek mt76_wmac detected, beacon interval override to [ %d ms ] (workaround for mt7603 beacon bug). Mesh mobility may be suboptimal.",
				      e->beacon_int);
	}

	if (e->phy[0] && mesh_phy_is_multi_band(e->phy)) {
		e->beacon_int = 100;
		if (log_info)
			mesh_log_info(ctx,
				      "Multi band wiphy detected, beacon interval override to [ %d ms ] (workaround for MLO bug). Mesh mobility may be suboptimal.",
				      e->beacon_int);
	}

	ctx->mesh_beacon_interval = e->beacon_int;
	ctx->mesh_dtim_period = e->dtim_period;
	e->valid_targets = 1;
	return 0;
}

static int phy_wide_differs(struct mesh_ctx *ctx, const char *phy,
			   const struct mobility_eval *e)
{
	int cur_rts, cur_lim, cur_q;
	int cur_aql_en = PHY_VAL_UNKNOWN, cur_aql = PHY_VAL_UNKNOWN;
	char aqlpath[160];

	if (!phy || !phy[0] || !e->valid_targets)
		return 0;
	phy_read_runtime_values(phy, &cur_rts, &cur_lim, &cur_q);
	if (!rts_is_unsupported(phy)) {
		if (cur_rts == PHY_VAL_UNKNOWN)
			rts_mark_unsupported(ctx, phy,
					     "cannot read RTS threshold");
		else if (cur_rts != e->rts)
			return 1;
	}
	if (cur_lim != e->limit || cur_q != e->quantum)
		return 1;
	snprintf(aqlpath, sizeof(aqlpath),
		 "/sys/kernel/debug/ieee80211/%s/aql_enable", phy);
	if (mesh_sysfs_read_int(aqlpath, &cur_aql_en) != 0)
		cur_aql_en = PHY_VAL_UNKNOWN;
	snprintf(aqlpath, sizeof(aqlpath),
		 "/sys/kernel/debug/ieee80211/%s/aql_threshold", phy);
	if (mesh_sysfs_read_int(aqlpath, &cur_aql) != 0)
		cur_aql = PHY_VAL_UNKNOWN;
	if (cur_aql_en != 1 || cur_aql != e->aql_threshold)
		return 1;
	return 0;
}

static int phy_apply_wide(struct mesh_ctx *ctx, const char *phy,
			  const struct mobility_eval *e)
{
	int cur_rts, cur_lim, cur_q;
	int cur_aql_en = PHY_VAL_UNKNOWN, cur_aql = PHY_VAL_UNKNOWN;
	int wrote = 0;
	char rts_s[16], lim_s[16], q_s[16];
	char aqlpath[160];
	char oldv[16];

	if (!phy || !phy[0] || !e->valid_targets)
		return 0;
	phy_read_runtime_values(phy, &cur_rts, &cur_lim, &cur_q);
	if (rts_is_unsupported(phy)) {
		/* Driver cannot report or honour RTS; never write it. */
	} else if (cur_rts == PHY_VAL_UNKNOWN) {
		rts_mark_unsupported(ctx, phy, "cannot read RTS threshold");
	} else if (cur_rts != e->rts) {
		char *a1[] = { "iw", "phy", (char *)phy, "set", "rts", rts_s, NULL };
		int after_rts, after_lim, after_q;

		snprintf(rts_s, sizeof(rts_s), "%d", e->rts);
		mesh_run_capture(a1, NULL, 0);
		phy_val_str(cur_rts, oldv, sizeof(oldv));
		mesh_log_info(ctx, "Old value:rts=%s, Setting new value:rts=%d",
			      oldv, e->rts);
		phy_read_runtime_values(phy, &after_rts, &after_lim, &after_q);
		if (after_rts == e->rts)
			wrote = 1;
		else
			rts_mark_unsupported(ctx, phy,
					     "write did not take effect");
	}
	if (cur_lim != e->limit) {
		char *a2[] = { "iw", "phy", (char *)phy, "set", "txq", "limit", lim_s, NULL };

		snprintf(lim_s, sizeof(lim_s), "%d", e->limit);
		mesh_run_capture(a2, NULL, 0);
		phy_val_str(cur_lim, oldv, sizeof(oldv));
		mesh_log_info(ctx, "Old value:txq_limit=%s, Setting new value:txq_limit=%d",
			      oldv, e->limit);
		wrote = 1;
	}
	if (cur_q != e->quantum) {
		char *a3[] = { "iw", "phy", (char *)phy, "set", "txq", "quantum", q_s, NULL };

		snprintf(q_s, sizeof(q_s), "%d", e->quantum);
		mesh_run_capture(a3, NULL, 0);
		phy_val_str(cur_q, oldv, sizeof(oldv));
		mesh_log_info(ctx,
			      "Old value:txq_quantum=%s, Setting new value:txq_quantum=%d",
			      oldv, e->quantum);
		wrote = 1;
	}
	snprintf(aqlpath, sizeof(aqlpath),
		 "/sys/kernel/debug/ieee80211/%s/aql_enable", phy);
	if (mesh_sysfs_read_int(aqlpath, &cur_aql_en) != 0)
		cur_aql_en = PHY_VAL_UNKNOWN;
	if (cur_aql_en != 1) {
		if (mesh_sysfs_write_int_if_changed(aqlpath, 1) == 1) {
			mesh_log_info(ctx,
				      "Old value:aql_enable=%d, Setting new value:aql_enable=1",
				      cur_aql_en);
			wrote = 1;
		}
	}
	snprintf(aqlpath, sizeof(aqlpath),
		 "/sys/kernel/debug/ieee80211/%s/aql_threshold", phy);
	if (mesh_sysfs_read_int(aqlpath, &cur_aql) != 0)
		cur_aql = PHY_VAL_UNKNOWN;
	if (cur_aql != e->aql_threshold) {
		if (mesh_sysfs_write_int_if_changed(aqlpath, e->aql_threshold) == 1) {
			mesh_log_info(ctx,
				      "Old value:aql_threshold=%d, Setting new value:aql_threshold=%d",
				      cur_aql, e->aql_threshold);
			wrote = 1;
		}
	}
	return wrote;
}

int mesh_mobility_phywide_differs(struct mesh_ctx *ctx)
{
	struct mobility_eval e;

	if (mobility_eval(ctx, &e, 0) != 0 || !e.valid_targets || !e.phy[0])
		return 0;
	return phy_wide_differs(ctx, e.phy, &e);
}

int mesh_mobility_apply_phywide(struct mesh_ctx *ctx)
{
	struct mobility_eval e;

	if (mobility_eval(ctx, &e, 0) != 0 || !e.valid_targets || !e.phy[0])
		return 0;
	return phy_apply_wide(ctx, e.phy, &e);
}

int mesh_sync_phywide_via_wifi(struct mesh_ctx *ctx)
{
	static int no_more_wifi;
	struct mobility_eval e;

	if (mobility_eval(ctx, &e, 0) != 0 || !e.valid_targets || !e.phy[0])
		return 0;
	if (!phy_wide_differs(ctx, e.phy, &e))
		return 0;
	if (no_more_wifi)
		return 0;
	phy_apply_wide(ctx, e.phy, &e);
	mesh_log_info(ctx,
		      "phy-wide settings changed; running wifi so vifs come up after them");
	{
		char *argv[] = { "wifi", NULL };
		mesh_run_capture(argv, NULL, 0);
	}
	mesh_wait_for_wifi_enabled(ctx);
	if (phy_wide_differs(ctx, e.phy, &e)) {
		no_more_wifi = 1;
		mesh_log_info(ctx,
			      "phy-wide settings still differ after wifi; not restarting wifi again");
	}
	return 1;
}

int mesh_phy_rts_display(const char *phy, char *out, size_t outlen)
{
	int rts, limit, quantum;

	if (!out || outlen == 0)
		return -1;
	if (!phy || !phy[0] || rts_is_unsupported(phy)) {
		snprintf(out, outlen, "unsupported");
		return 0;
	}
	phy_read_runtime_values(phy, &rts, &limit, &quantum);
	if (rts == PHY_VAL_UNKNOWN)
		snprintf(out, outlen, "unsupported");
	else
		phy_val_str(rts, out, outlen);
	return 0;
}

int mesh_mobility_settings(struct mesh_ctx *ctx, const char *mode)
{
	struct mobility_eval e;
	char setl[192];
	char content[384];
	int do_config, do_phy, do_runtime;

	do_config = (!mode || !strcmp(mode, "config") || !strcmp(mode, "all"));
	do_phy = (!mode || !strcmp(mode, "phy") || !strcmp(mode, "parameters") ||
		  !strcmp(mode, "all"));
	do_runtime = (!mode || !strcmp(mode, "runtime") || !strcmp(mode, "all"));

	if (mobility_eval(ctx, &e, do_config) != 0)
		return 0;

	if (e.level == 0) {
		if (do_config) {
			if (e.radio[0]) {
				snprintf(setl, sizeof(setl), "del wireless.%s.beacon_int", e.radio);
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
	}

	if (e.phy[0] && do_phy)
		phy_apply_wide(ctx, e.phy, &e);

	if (do_config) {
		/* v7 ~4450: write beacon_int only on the mesh radio. MLO may force
		 * beacon_int=100 above, but sibling radios stay unset (hostapd
		 * default 100) — baseline-v7-fd4d has radio0.beacon_int=100 only. */
		if (e.radio[0]) {
			snprintf(setl, sizeof(setl), "del wireless.%s.beacon_int", e.radio);
			mesh_uci_batch_line(setl);
			snprintf(setl, sizeof(setl), "set wireless.%s.beacon_int='%d'",
				 e.radio, e.beacon_int);
			mesh_uci_batch_line(setl);
		}
		if (e.stanza[0]) {
			snprintf(setl, sizeof(setl), "set %s.dtim_period='%d'",
				 e.stanza, e.dtim_period);
			mesh_uci_batch_line(setl);
		}
		snprintf(setl, sizeof(setl), "set mesh11sd.mesh_params.mesh_fwding='%d'",
			 e.mesh_fwding);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_hwmp_rann_interval='%d'", e.rann);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_hwmp_preq_min_interval='%d'", e.preq_min);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_hwmp_active_path_timeout='%d'",
			 e.path_timeout);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_max_peer_links='%d'", e.max_peers);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_rssi_threshold='%s'", e.rssi);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_hwmp_max_preq_retries='%d'", e.max_preq);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.mesh_params.mesh_hwmp_rootmode='%d'", e.rootmode);
		mesh_uci_batch_line(setl);
	}

	/* path_cost is per-vif: only after vifs are up (runtime), not with config/phy */
	if (do_runtime) {
		char br[64];
		snprintf(br, sizeof(br), "br-%s", ctx->auto_mesh_network);
		mesh_set_iface_path_cost(ctx, ctx->mesh_path_cost, br,
					 ctx->active_mesh_ifname[0] ? ctx->active_mesh_ifname : NULL);
	}

	snprintf(content, sizeof(content),
		 "mesh_node_mobility_level=\"%d\"; rootmode=\"%d\"; beacon_int=\"%d\"; "
		 "dtim_period=\"%d\"; rts=\"%d\"; limit=\"%d\"; quantum=\"%d\"; "
		 "aql_threshold=\"%d\"\n",
		 e.level, e.rootmode, e.beacon_int, e.dtim_period, e.rts, e.limit,
		 e.quantum, e.aql_threshold);
	mesh_file_write(MESH11SD_TMPDIR "/mesh_mobility", content);
	mesh_log_debug(ctx, "mobility_settings level=%d rootmode=%d beacon=%d driver=%s",
		       e.level, e.rootmode, e.beacon_int,
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
	mesh_manage_fallback_ula_route(ctx);
	if (ctx->link_to_portal == 0) {
		mesh_log_debug(ctx, "Check Channel - portal detected");
		return 0;
	}
	mesh_log_debug(ctx, "Check Channel - portal not detected - Scanning...");
	mesh_scan_channel(ctx);
	return 0;
}
