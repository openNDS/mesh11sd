/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void mesh_ctx_init(struct mesh_ctx *ctx, int is_daemon)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->debuglevel = 3;
	ctx->enabled = 1;
	ctx->checkinterval = 10;
	ctx->loop_interval_delta = 20;
	ctx->max_log_entries = 500;
	ctx->interface_timeout = 10;
	ctx->portal_detect = 1;
	ctx->portal_detect_threshold = 5;
	ctx->mesh_path_cost = 65525;
	ctx->mesh_metric_threshold = 50;
	ctx->ssid_suffix_enable = 1;
	ctx->mesh_gate_encryption = 4;
	ctx->mesh_gate_enable = 1;
	ctx->vtun_gate_encryption = 4;
	ctx->mesh_node_mobility_level = 2;
	ctx->mesh_dtim_period = 2;
	ctx->mesh_beacon_interval = 25;
	ctx->mesh_rssi_threshold = -63;
	ctx->odhcpd_log_level = 3;
	ctx->mute = is_daemon ? 0 : 1;
	ctx->pid = getpid();
	snprintf(ctx->log_mountpoint, sizeof(ctx->log_mountpoint), "/tmp");
	snprintf(ctx->auto_mesh_band, sizeof(ctx->auto_mesh_band), "2g40");
	snprintf(ctx->auto_mesh_network, sizeof(ctx->auto_mesh_network), "lan");
	snprintf(ctx->mesh_basename, sizeof(ctx->mesh_basename), "m-11s-");
	snprintf(ctx->portal_channel, sizeof(ctx->portal_channel), "default");
	snprintf(ctx->tun_id, sizeof(ctx->tun_id), "69");
	snprintf(ctx->vtun_base_ssid, sizeof(ctx->vtun_base_ssid), "VTunnel");
	snprintf(ctx->detected_state, sizeof(ctx->detected_state), "peer");
	snprintf(ctx->last_state, sizeof(ctx->last_state), "initial");
	ctx->firstloop = 1;
	ctx->macfixup = 0;
	ctx->daemon_startup = 1;
	ctx->link_to_portal = 1;
	ctx->reboot_on_error = 1;
	ctx->stop_on_error = 0;
	mesh_mkdir_p(MESH11SD_TMPDIR);
}

static int parse_int_default(const char *s, int def)
{
	char *end;
	long v;

	if (!s || !s[0])
		return def;
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s)
		return def;
	return (int)v;
}

static void hash_mesh_id(struct mesh_ctx *ctx, const char *raw)
{
	char full[80];
	char hash[72];
	const char *src = (raw && raw[0]) ? raw : "--__";
	size_t n;

	snprintf(ctx->auto_mesh_id_raw, sizeof(ctx->auto_mesh_id_raw), "%s", src);
	if (mesh_sha256_hex(src, hash, sizeof(hash)) != 0) {
		snprintf(ctx->auto_mesh_id, sizeof(ctx->auto_mesh_id), "%s", src);
		return;
	}
	/* v7: tail -c55 | head -c30 of the hex digest */
	n = strlen(hash);
	if (n > 55)
		n = 55;
	{
		size_t start = (n > 30) ? (n - 30) : 0;
		/* Actually: printf hash | tail -c55 | head -c30
		 * sha256 is 64 chars. tail -c55 takes last 55, head -c30 takes first 30 of those.
		 * start = 64-55 = 9, then 30 chars → hash[9..38]
		 */
		start = (strlen(hash) >= 55) ? (strlen(hash) - 55) : 0;
		snprintf(ctx->auto_mesh_id, sizeof(ctx->auto_mesh_id), "%.30s", hash + start);
	}
	(void)full;
}

static void hash_mesh_key(struct mesh_ctx *ctx, const char *raw_key)
{
	char seed[256];
	char hash[72];
	const char *default_suffix =
		"91b39a9b41e918e9bce1c8d5d7d3e071d6f1f8855a7c0214f687550177c5d5b8";

	if (!raw_key || !raw_key[0])
		snprintf(seed, sizeof(seed), "%s%s", ctx->auto_mesh_id, default_suffix);
	else
		snprintf(seed, sizeof(seed), "%s%s", ctx->auto_mesh_id, raw_key);
	/* v7: echo "$auto_mesh_id""$suffix" | sha256sum — newline in digest */
	if (mesh_sha256_hex_echo(seed, hash, sizeof(hash)) == 0)
		snprintf(ctx->auto_mesh_key, sizeof(ctx->auto_mesh_key), "%s", hash);
	else
		snprintf(ctx->auto_mesh_key, sizeof(ctx->auto_mesh_key), "%s", seed);
}

int mesh_ctx_load_setup(struct mesh_ctx *ctx)
{
	char buf[256];
	int saved_mute = ctx->mute;

	ctx->mute = ctx->mute; /* keep */
	mesh_write_to_syslog(ctx, "notice", "Reading configuration");

	if (mesh_uci_get("mesh11sd.setup.debuglevel", buf, sizeof(buf)) == 0)
		ctx->debuglevel = parse_int_default(buf, 3);
	else
		ctx->debuglevel = 3;
	if (ctx->debuglevel < 0)
		ctx->debuglevel = 0;
	if (ctx->debuglevel > 3)
		ctx->debuglevel = 3;

	if (mesh_uci_get("mesh11sd.setup.enabled", buf, sizeof(buf)) == 0)
		ctx->enabled = parse_int_default(buf, 1);
	else
		ctx->enabled = 1;
	mesh_log_info(ctx, "option enabled [ %d ]", ctx->enabled);

	if (mesh_uci_get("mesh11sd.setup.log_mountpoint", buf, sizeof(buf)) == 0 && buf[0]) {
		strncpy(ctx->log_mountpoint, buf, sizeof(ctx->log_mountpoint) - 1);
		ctx->log_mountpoint[sizeof(ctx->log_mountpoint) - 1] = '\0';
	} else {
		strncpy(ctx->log_mountpoint, "/tmp", sizeof(ctx->log_mountpoint) - 1);
	}
	mesh_mkdir_p(ctx->log_mountpoint);
	{
		char logdir[192];
		snprintf(logdir, sizeof(logdir), "%s/mesh11sd", ctx->log_mountpoint);
		mesh_mkdir_p(logdir);
	}
	mesh_mkdir_p(MESH11SD_TMPDIR);
	mesh_log_info(ctx, "option log_mountpoint [ %s ]", ctx->log_mountpoint);

	if (mesh_uci_get("mesh11sd.setup.max_log_entries", buf, sizeof(buf)) == 0)
		ctx->max_log_entries = parse_int_default(buf, 500);
	else
		ctx->max_log_entries = 500;
	mesh_log_info(ctx, "option max_log_entries [ %d ]", ctx->max_log_entries);
	mesh_log_info(ctx, "option debuglevel [ %d ]", ctx->debuglevel);

	if (mesh_uci_get("mesh11sd.setup.checkinterval", buf, sizeof(buf)) == 0)
		ctx->checkinterval = parse_int_default(buf, 10);
	else
		ctx->checkinterval = 10;
	if (ctx->checkinterval < 10)
		ctx->checkinterval = 10;
	mesh_log_info(ctx, "option checkinterval [ %d ]", ctx->checkinterval);

	if (mesh_uci_get("mesh11sd.setup.loop_interval_delta", buf, sizeof(buf)) == 0)
		ctx->loop_interval_delta = parse_int_default(buf, 20);
	else
		ctx->loop_interval_delta = 20;
	mesh_log_info(ctx, "option loop_interval_delta [ %d ]", ctx->loop_interval_delta);

	if (mesh_uci_get("mesh11sd.setup.commit_all", buf, sizeof(buf)) == 0)
		ctx->commit_all = parse_int_default(buf, 0);
	else
		ctx->commit_all = 0;
	mesh_log_info(ctx, "option commit_all [ %d ]", ctx->commit_all);

	if (mesh_uci_get("mesh11sd.setup.portal_detect", buf, sizeof(buf)) == 0)
		ctx->portal_detect = parse_int_default(buf, 1);
	else
		ctx->portal_detect = 1;
	if (ctx->portal_detect == 2)
		ctx->portal_detect = 1;
	mesh_log_info(ctx, "option portal_detect [ %d ]", ctx->portal_detect);

	if (mesh_uci_get("mesh11sd.setup.portal_use_default_ipv4", buf, sizeof(buf)) == 0)
		ctx->portal_use_default_ipv4 = parse_int_default(buf, 0);
	else
		ctx->portal_use_default_ipv4 = 0;

	if (mesh_uci_get("mesh11sd.setup.portal_detect_threshold", buf, sizeof(buf)) == 0)
		ctx->portal_detect_threshold = parse_int_default(buf, 5);
	else
		ctx->portal_detect_threshold = 5;
	if (ctx->portal_detect == 0)
		ctx->portal_detect_threshold = 0;

	if (mesh_uci_get("mesh11sd.setup.mesh_path_cost", buf, sizeof(buf)) == 0)
		ctx->mesh_path_cost = parse_int_default(buf, 65525);
	else
		ctx->mesh_path_cost = 65525;
	ctx->mesh_metric_threshold = 50;
	mesh_log_info(ctx, "option mesh_path_cost [ %d ]", ctx->mesh_path_cost);
	if (mesh_uci_get("mesh11sd.setup.mesh_metric_threshold", buf, sizeof(buf)) == 0)
		ctx->mesh_metric_threshold = parse_int_default(buf, 50);
	else
		ctx->mesh_metric_threshold = 50;
	mesh_log_info(ctx, "option mesh_metric_threshold [ %d ]", ctx->mesh_metric_threshold);

	if (mesh_uci_get("mesh11sd.setup.interface_timeout", buf, sizeof(buf)) == 0)
		ctx->interface_timeout = parse_int_default(buf, 10);
	else
		ctx->interface_timeout = 10;
	if (ctx->interface_timeout < 10)
		ctx->interface_timeout = 10;

	if (mesh_uci_get("mesh11sd.setup.auto_config", buf, sizeof(buf)) == 0)
		ctx->auto_config = parse_int_default(buf, 0);
	else
		ctx->auto_config = 0;
	if (ctx->auto_config > 2)
		ctx->auto_config = 1;
	mesh_log_info(ctx, "option auto_config [ %d ]", ctx->auto_config);

	if (mesh_uci_get("mesh11sd.setup.auto_mesh_id", buf, sizeof(buf)) != 0 || !buf[0])
		snprintf(buf, sizeof(buf), "--__");
	hash_mesh_id(ctx, buf);
	mesh_log_info(ctx, "auto_mesh_id hash [ %s ]", ctx->auto_mesh_id);

	if (mesh_uci_get("mesh11sd.setup.auto_mesh_band", buf, sizeof(buf)) == 0 && buf[0])
		snprintf(ctx->auto_mesh_band, sizeof(ctx->auto_mesh_band), "%s", buf);
	else
		snprintf(ctx->auto_mesh_band, sizeof(ctx->auto_mesh_band), "2g40");
	{
		const char *ok[] = { "2g", "2g40", "5g", "6g", "60g", NULL };
		int i, good = 0;
		for (i = 0; ok[i]; i++)
			if (!strcmp(ctx->auto_mesh_band, ok[i]))
				good = 1;
		if (!good)
			snprintf(ctx->auto_mesh_band, sizeof(ctx->auto_mesh_band), "2g40");
	}
	mesh_log_info(ctx, "option auto_mesh_band [ %s ]", ctx->auto_mesh_band);

	if (mesh_uci_get("mesh11sd.setup.portal_channel", buf, sizeof(buf)) == 0 && buf[0])
		snprintf(ctx->portal_channel, sizeof(ctx->portal_channel), "%s", buf);
	else
		snprintf(ctx->portal_channel, sizeof(ctx->portal_channel), "default");

	if (mesh_uci_get("mesh11sd.setup.mesh_phy_index", buf, sizeof(buf)) == 0)
		snprintf(ctx->mesh_phy_index, sizeof(ctx->mesh_phy_index), "%s", buf);
	else
		ctx->mesh_phy_index[0] = '\0';

	if (mesh_uci_get("mesh11sd.setup.mesh_multi_interfaces", buf, sizeof(buf)) == 0 &&
	    !strcmp(buf, "enabled"))
		ctx->mesh_multi_interfaces_enabled = 1;
	else
		ctx->mesh_multi_interfaces_enabled = 0;

	if (mesh_uci_get("mesh11sd.setup.country", buf, sizeof(buf)) == 0)
		snprintf(ctx->country, sizeof(ctx->country), "%s", buf);
	else
		ctx->country[0] = '\0';
	if (ctx->country[0])
		mesh_log_info(ctx, "option country [ %s ]", ctx->country);

	if (mesh_uci_get("mesh11sd.setup.auto_mesh_key", buf, sizeof(buf)) == 0)
		hash_mesh_key(ctx, buf);
	else
		hash_mesh_key(ctx, NULL);
	mesh_log_info(ctx, "option auto_mesh_key [ set ]");

	if (mesh_uci_get("mesh11sd.setup.auto_mesh_network", buf, sizeof(buf)) == 0 && buf[0])
		snprintf(ctx->auto_mesh_network, sizeof(ctx->auto_mesh_network), "%s", buf);
	else
		snprintf(ctx->auto_mesh_network, sizeof(ctx->auto_mesh_network), "lan");
	if (ctx->portal_detect != 3) {
		if (!strcmp(ctx->auto_mesh_network, "wan") ||
		    !strcmp(ctx->auto_mesh_network, "wan6"))
			snprintf(ctx->auto_mesh_network, sizeof(ctx->auto_mesh_network), "lan");
	} else if (!strcmp(ctx->auto_mesh_network, "lan")) {
		snprintf(ctx->auto_mesh_network, sizeof(ctx->auto_mesh_network), "wan");
	}
	mesh_log_info(ctx, "option auto_mesh_network [ %s ]", ctx->auto_mesh_network);

	if (mesh_uci_get("mesh11sd.setup.mesh_basename", buf, sizeof(buf)) == 0 && buf[0]) {
		char cleaned[32];
		int j = 0, i;
		for (i = 0; buf[i] && j < 4; i++) {
			if (isalnum((unsigned char)buf[i]))
				cleaned[j++] = (char)tolower((unsigned char)buf[i]);
		}
		cleaned[j] = '\0';
		snprintf(ctx->mesh_basename, sizeof(ctx->mesh_basename), "m-%s-", cleaned);
	} else {
		snprintf(ctx->mesh_basename, sizeof(ctx->mesh_basename), "m-11s-");
	}

	if (mesh_uci_get("mesh11sd.setup.ssid_suffix_enable", buf, sizeof(buf)) == 0)
		ctx->ssid_suffix_enable = parse_int_default(buf, 1);
	else
		ctx->ssid_suffix_enable = 1;

	if (mesh_uci_get("mesh11sd.setup.mesh_gate_base_ssid", buf, sizeof(buf)) == 0) {
		int maxlen = ctx->ssid_suffix_enable ? 22 : 30;
		int i, j = 0;
		for (i = 0; buf[i] && j < maxlen; i++) {
			if (buf[i] != ' ')
				ctx->mesh_gate_base_ssid[j++] = buf[i];
		}
		ctx->mesh_gate_base_ssid[j] = '\0';
	} else {
		ctx->mesh_gate_base_ssid[0] = '\0';
	}
	if (!ctx->mesh_gate_base_ssid[0])
		snprintf(ctx->mesh_gate_base_ssid, sizeof(ctx->mesh_gate_base_ssid), "MeshGate");

	{
		const char *wpad_full[] = {
			"wpad-mbedtls", "wpad-wolfssl", "wpad-openssl", NULL
		};
		const char *wpad_mesh[] = {
			"wpad-mesh-mbedtls", "wpad-mesh-wolfssl", "wpad-mesh-openssl",
			"wpad-mbedtls", "wpad-wolfssl", "wpad-openssl", NULL
		};
		int flags[8] = {0};
		int no_full = mesh_check_package_any(wpad_full, flags);
		if (mesh_uci_get("mesh11sd.setup.mesh_gate_encryption", buf, sizeof(buf)) == 0)
			ctx->mesh_gate_encryption = parse_int_default(buf, 4);
		else
			ctx->mesh_gate_encryption = 4;
		if (no_full) {
			int no_mesh = mesh_check_package_any(wpad_mesh, NULL);
			if (no_mesh)
				ctx->mesh_gate_encryption = 0;
		}
		if (ctx->mesh_gate_encryption == 4 && no_full)
			ctx->mesh_gate_encryption = 0;
	}
	mesh_log_info(ctx, "option mesh_gate_encryption [ %d ]", ctx->mesh_gate_encryption);

	if (mesh_uci_get("mesh11sd.setup.mesh_gate_key", buf, sizeof(buf)) == 0)
		snprintf(ctx->mesh_gate_key, sizeof(ctx->mesh_gate_key), "%s", buf);
	else
		ctx->mesh_gate_key[0] = '\0';
	if (ctx->mesh_gate_encryption == 4)
		snprintf(ctx->mesh_gate_key, sizeof(ctx->mesh_gate_key), "owe_null_key");
	if (ctx->mesh_gate_key[0] && strlen(ctx->mesh_gate_key) < 8 &&
	    ctx->mesh_gate_encryption != 4) {
		mesh_log_err(ctx, "mesh_gate_key is too short, it must be 8 or more characters");
		ctx->mesh_gate_key[0] = '\0';
	}
	mesh_log_info(ctx, "option mesh_gate_key [ %s ]",
		      ctx->mesh_gate_key[0] ? "set" : "unset");

	{
		const char *vx[] = { "ip-full", "vxlan", NULL };
		int flags[2] = {1, 1};
		mesh_check_package_any(vx, flags);
		if (flags[0] == 0 && flags[1] == 0) {
			if (mesh_uci_get("mesh11sd.setup.vtun_enable", buf, sizeof(buf)) == 0)
				ctx->vtun_enable = parse_int_default(buf, 1);
			else
				ctx->vtun_enable = 1;
			if (ctx->vtun_enable > 1)
				ctx->vtun_enable = 1;
		} else {
			ctx->vtun_enable = 0;
			mesh_log_info(ctx, "option vtun_enable is disabled because required dependencies [ ip-full vxlan ] are not met");
		}
	}
	if (mesh_uci_get("mesh11sd.setup.tun_id", buf, sizeof(buf)) == 0 && buf[0])
		snprintf(ctx->tun_id, sizeof(ctx->tun_id), "%s", buf);
	if (mesh_uci_get("mesh11sd.setup.vtun_base_ssid", buf, sizeof(buf)) == 0 && buf[0])
		snprintf(ctx->vtun_base_ssid, sizeof(ctx->vtun_base_ssid), "%s", buf);
	if (mesh_uci_get("mesh11sd.setup.vtun_gate_encryption", buf, sizeof(buf)) == 0)
		ctx->vtun_gate_encryption = parse_int_default(buf, 4);
	else
		ctx->vtun_gate_encryption = 4;
	if (mesh_uci_get("mesh11sd.setup.vtun_gate_key", buf, sizeof(buf)) == 0)
		snprintf(ctx->vtun_gate_key, sizeof(ctx->vtun_gate_key), "%s", buf);
	else
		ctx->vtun_gate_key[0] = '\0';
	if (ctx->vtun_gate_encryption == 4)
		snprintf(ctx->vtun_gate_key, sizeof(ctx->vtun_gate_key), "owe_null_key");

	if (mesh_uci_get("mesh11sd.setup.mesh_gate_enable", buf, sizeof(buf)) == 0)
		ctx->mesh_gate_enable = parse_int_default(buf, 1);
	else
		ctx->mesh_gate_enable = 1;

	if (mesh_uci_get("mesh11sd.setup.mesh_leechmode_enable", buf, sizeof(buf)) != 0)
		mesh_uci_get("mesh11sd.setup.mesh_gate_only", buf, sizeof(buf));
	ctx->mesh_gate_only = parse_int_default(buf, 0);
	if (ctx->mesh_gate_only == 1 && ctx->mesh_gate_enable == 0)
		ctx->mesh_gate_only = 0;

	if (mesh_uci_get("mesh11sd.setup.txpower", buf, sizeof(buf)) == 0)
		snprintf(ctx->txpower, sizeof(ctx->txpower), "%s", buf);
	else
		ctx->txpower[0] = '\0';

	if (mesh_uci_get("mesh11sd.setup.mesh_dtim_period", buf, sizeof(buf)) == 0)
		ctx->mesh_dtim_period = parse_int_default(buf, 2);
	else
		ctx->mesh_dtim_period = 2;

	if (mesh_uci_get("mesh11sd.setup.mesh_node_mobility_level", buf, sizeof(buf)) == 0)
		ctx->mesh_node_mobility_level = parse_int_default(buf, 2);
	else
		ctx->mesh_node_mobility_level = 2;

	if (mesh_uci_get("mesh11sd.setup.use_default_beacon_interval", buf, sizeof(buf)) == 0)
		ctx->use_default_beacon_interval = parse_int_default(buf, 0) > 0 ? 1 : 0;
	else
		ctx->use_default_beacon_interval = 0;

	/* beacon from mobility level 2 default unless overridden later */
	{
		static const int beacons[] = { 100, 50, 25, 20, 15, 15, 15 };
		int lvl = ctx->mesh_node_mobility_level;
		if (lvl < 0 || lvl > 6)
			lvl = 2;
		ctx->mesh_beacon_interval = ctx->use_default_beacon_interval ? 100 : beacons[lvl];
	}

	if (mesh_uci_get("mesh11sd.setup.odhcpd_log_level", buf, sizeof(buf)) == 0)
		ctx->odhcpd_log_level = parse_int_default(buf, 3);
	else
		ctx->odhcpd_log_level = 3;

	if (mesh_uci_get("mesh11sd.setup.reboot_on_error", buf, sizeof(buf)) == 0)
		ctx->reboot_on_error = parse_int_default(buf, 1) ? 1 : 0;
	else
		ctx->reboot_on_error = 1;
	if (mesh_uci_get("mesh11sd.setup.stop_on_error", buf, sizeof(buf)) == 0)
		ctx->stop_on_error = parse_int_default(buf, 0) ? 1 : 0;
	else
		ctx->stop_on_error = 0;
	if (mesh_uci_get("mesh11sd.setup.swconfig_mac_skip", buf, sizeof(buf)) == 0)
		ctx->swconfig_mac_skip = !strcmp(buf, "1") ? 1 : 0;
	else
		ctx->swconfig_mac_skip = 0;

	if (mesh_uci_get("mesh11sd.setup.mesh_backhaul_led", buf, sizeof(buf)) == 0 && buf[0])
		snprintf(ctx->mesh_backhaul_led, sizeof(ctx->mesh_backhaul_led), "%s", buf);
	else
		snprintf(ctx->mesh_backhaul_led, sizeof(ctx->mesh_backhaul_led), "auto");
	if (mesh_uci_get("mesh11sd.setup.mesh_mac_forced_forwarding", buf, sizeof(buf)) == 0)
		ctx->mesh_mac_forced_forwarding = parse_int_default(buf, 1);
	else
		ctx->mesh_mac_forced_forwarding = 1;
	if (mesh_uci_get("mesh11sd.setup.gateway_proxy_arp", buf, sizeof(buf)) == 0)
		ctx->gateway_proxy_arp = parse_int_default(buf, 1);
	else
		ctx->gateway_proxy_arp = 1;
	if (mesh_uci_get("mesh11sd.setup.watchdog_nonvolatile_log", buf, sizeof(buf)) == 0)
		ctx->watchdog_nonvolatile_log = parse_int_default(buf, 0);
	else
		ctx->watchdog_nonvolatile_log = 0;
	if (mesh_uci_get("mesh11sd.setup.mesh_path_stabilisation", buf, sizeof(buf)) == 0)
		ctx->mesh_path_stabilisation = parse_int_default(buf, 1);
	else
		ctx->mesh_path_stabilisation = 1;
	if (mesh_uci_get("mesh11sd.setup.reactive_path_stabilisation_threshold", buf, sizeof(buf)) == 0)
		ctx->reactive_path_stabilisation_threshold = parse_int_default(buf, 0);
	else
		ctx->reactive_path_stabilisation_threshold = 0;
	if (mesh_uci_get("mesh11sd.setup.vtun_ip", buf, sizeof(buf)) == 0 && buf[0])
		snprintf(ctx->vtun_ip, sizeof(ctx->vtun_ip), "%s", buf);
	if (mesh_uci_get("mesh11sd.setup.vtun_mask", buf, sizeof(buf)) == 0 && buf[0])
		snprintf(ctx->vtun_mask, sizeof(ctx->vtun_mask), "%s", buf);
	if (mesh_uci_get("mesh11sd.setup.vtun_path_cost", buf, sizeof(buf)) == 0)
		ctx->vtun_path_cost = parse_int_default(buf, 65525);
	if (mesh_uci_get("mesh11sd.setup.vtun_isolate", buf, sizeof(buf)) == 0)
		ctx->vtun_isolate = parse_int_default(buf, 0);
	if (mesh_uci_get("mesh11sd.setup.move_ethernet_to_vxlan_bridge", buf, sizeof(buf)) == 0)
		ctx->move_ethernet_to_vxlan_bridge = parse_int_default(buf, 0);
	if (mesh_uci_get("mesh11sd.setup.cpe_mode", buf, sizeof(buf)) == 0 && buf[0])
		snprintf(ctx->cpe_mode, sizeof(ctx->cpe_mode), "%s", buf);
	snprintf(ctx->mesh_backhaul_led, sizeof(ctx->mesh_backhaul_led), "auto");
	ctx->mesh_driver[0] = '\0';
	ctx->mesh_mac_forced_forwarding = 1;
	ctx->gateway_proxy_arp = 1;
	ctx->watchdog_nonvolatile_log = 0;
	ctx->mesh_path_stabilisation = 1;
	ctx->reactive_path_stabilisation_threshold = 0;
	snprintf(ctx->vtun_ip, sizeof(ctx->vtun_ip), "auto");
	snprintf(ctx->vtun_mask, sizeof(ctx->vtun_mask), "255.255.255.0");
	ctx->vtun_path_cost = 65525;
	ctx->vtun_isolate = 0;
	ctx->move_ethernet_to_vxlan_bridge = 0;
	snprintf(ctx->cpe_mode, sizeof(ctx->cpe_mode), "auto");

	(void)saved_mute;
	return 0;
}

int mesh_check_config_params(struct mesh_ctx *ctx)
{
	char buf[64];
	int mesh_fwding = 1;
	int mesh_retry = 255, mesh_confirm = 255, mesh_holding = 255;
	int gate_ann = 1;

	if (mesh_uci_get("mesh11sd.mesh_params.mesh_fwding", buf, sizeof(buf)) == 0)
		mesh_fwding = parse_int_default(buf, 1);
	if (mesh_fwding == 1 && ctx->mesh_gate_only == 1)
		mesh_fwding = 0;
	mesh_uci_set("mesh11sd.mesh_params.mesh_fwding", mesh_fwding ? "1" : "0");

	if (mesh_uci_get("mesh11sd.mesh_params.mesh_retry_timeout", buf, sizeof(buf)) == 0)
		mesh_retry = parse_int_default(buf, 255);
	snprintf(buf, sizeof(buf), "%d", mesh_retry);
	mesh_uci_set("mesh11sd.mesh_params.mesh_retry_timeout", buf);

	if (mesh_uci_get("mesh11sd.mesh_params.mesh_confirm_timeout", buf, sizeof(buf)) == 0)
		mesh_confirm = parse_int_default(buf, 255);
	snprintf(buf, sizeof(buf), "%d", mesh_confirm);
	mesh_uci_set("mesh11sd.mesh_params.mesh_confirm_timeout", buf);

	if (mesh_uci_get("mesh11sd.mesh_params.mesh_holding_timeout", buf, sizeof(buf)) == 0)
		mesh_holding = parse_int_default(buf, 255);
	snprintf(buf, sizeof(buf), "%d", mesh_holding);
	mesh_uci_set("mesh11sd.mesh_params.mesh_holding_timeout", buf);

	if (mesh_uci_get("mesh11sd.mesh_params.mesh_rssi_threshold", buf, sizeof(buf)) == 0)
		ctx->mesh_rssi_threshold = parse_int_default(buf, -63);
	else
		ctx->mesh_rssi_threshold = -63;
	if (ctx->mesh_rssi_threshold > 0)
		ctx->mesh_rssi_threshold = -63;
	snprintf(buf, sizeof(buf), "%d", ctx->mesh_rssi_threshold);
	mesh_uci_set("mesh11sd.mesh_params.mesh_rssi_threshold", buf);
	mesh_log_info(ctx, "option mesh_rssi_threshold [ %d ]", ctx->mesh_rssi_threshold);

	if (mesh_uci_get("mesh11sd.mesh_params.mesh_gate_announcements", buf, sizeof(buf)) == 0)
		gate_ann = parse_int_default(buf, 1);
	if (gate_ann == 1 && ctx->mesh_gate_enable == 0)
		gate_ann = 0;
	if (ctx->mesh_gate_only > 0)
		gate_ann = 0;
	mesh_uci_set("mesh11sd.mesh_params.mesh_gate_announcements", gate_ann ? "1" : "0");

	if (mesh_uci_get("mesh11sd.mesh_params.mesh_hwmp_rootmode", buf, sizeof(buf)) != 0)
		mesh_uci_set("mesh11sd.mesh_params.mesh_hwmp_rootmode", "0");

	return 0;
}
