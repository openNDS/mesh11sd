/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH11SD_H
#define MESH11SD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef MESH11SD_VERSION
#define MESH11SD_VERSION "8.0.0_alpha2"
#endif

#define MESH11SD_TMPDIR "/tmp/mesh11sd"
#define MESH11SD_CONFIG "mesh11sd"
#define MESH11SD_LOG_NAME "mesh11sd_log.log"
#define MESH11SD_MAX_IFACES 16
#define MESH11SD_MAX_RADIOS 8

struct mesh_ctx {
	int debuglevel;
	int enabled;
	int commit_all;
	int checkinterval;
	int loop_interval_delta;
	int max_log_entries;
	int interface_timeout;
	int auto_config;
	int portal_detect;
	int portal_use_default_ipv4;
	int portal_detect_threshold;
	int channel_tracking_checkinterval;
	int mesh_path_cost;
	int ssid_suffix_enable;
	int mesh_gate_encryption;
	int mesh_gate_enable;
	int mesh_gate_only;
	int vtun_enable;
	int vtun_gate_encryption;
	int mesh_multi_interfaces_enabled;
	int mesh_node_mobility_level;
	int mesh_dtim_period;
	int mesh_beacon_interval;
	int use_default_beacon_interval;
	int mesh_rssi_threshold;
	int odhcpd_log_level;
	int mute;
	unsigned long entrynum;
	pid_t pid;

	char log_mountpoint[128];
	char last_syslogmessage[512];
	char auto_mesh_id[64];		/* hashed id used as mesh_id */
	char auto_mesh_id_raw[128];
	char auto_mesh_key[80];		/* hashed key - never print */
	char auto_mesh_band[16];
	char auto_mesh_network[32];
	char mesh_basename[32];
	char portal_channel[32];
	char country[8];
	char mesh_phy_index[8];
	char mesh_gate_base_ssid[64];
	char mesh_gate_key[128];		/* never print */
	char vtun_base_ssid[64];
	char vtun_gate_key[128];		/* never print */
	char tun_id[16];
	char txpower[16];
	char factory_mac[32];
	char device[64];
	char devicemac[32];
	char active_mesh_ifname[64];

	/* portal/peer runtime (tmpdir/is_portal etc.) */
	char detected_state[16];	/* portal|peer */
	char last_state[16];	/* initial|portal|peer */
	char is_portal[32];
	char portal_ula[64];
	char ula_prefix[48];
	char ula_base[32];
	char ula_subnet[8];
	char ula_vxsubnet[8];
	char portal_type[16];
	int link_to_portal;
	int failcount;
	int network_restart;
	int firstloop;
	int changed;
	int daemon_startup;
	int leechmode;
	int reboot_on_error;
	int stop_on_error;
	int swconfig_mac_skip;

	char iflist[MESH11SD_MAX_IFACES][32];
	int iflist_count;
	char radios[MESH11SD_MAX_RADIOS][32];
	int radio_count;
	char wiphys[MESH11SD_MAX_RADIOS][16];
	int wiphy_count;
};

/* util */
int mesh_run_capture(char *const argv[], char *out, size_t outlen);
int mesh_sha256_hex(const char *in, char *out, size_t outlen);
int mesh_is_installed(const char *package);
int mesh_check_package_any(const char *const *names, int *installed_flags);
int mesh_file_write(const char *path, const char *content);
int mesh_mkdir_p(const char *path);

/* uci_ops: v7 uncommitted default; commit_all mode honoured by callers */
int mesh_uci_get(const char *tuple, char *buf, size_t buflen);
int mesh_uci_set(const char *tuple, const char *value);
int mesh_uci_add(const char *tuple); /* uci add ... */
int mesh_uci_commit(const char *package);
int mesh_uci_revert(const char *package);
int mesh_uci_changes(const char *package, char *buf, size_t buflen);
int mesh_uci_batch_line(const char *line); /* "set foo.bar='baz'" */
int mesh_uci_show_filter(const char *package, const char *grep_pat,
			 char *buf, size_t buflen);

void mesh_ctx_init(struct mesh_ctx *ctx, int is_daemon);
int mesh_ctx_load_setup(struct mesh_ctx *ctx);
int mesh_check_config_params(struct mesh_ctx *ctx);

void mesh_write_to_syslog(struct mesh_ctx *ctx, const char *debugtype, const char *msg);
void mesh_write_log(struct mesh_ctx *ctx, const char *debugtype, const char *msg);
int mesh_read_log(struct mesh_ctx *ctx, int follow);
void mesh_log_info(struct mesh_ctx *ctx, const char *fmt, ...);
void mesh_log_debug(struct mesh_ctx *ctx, const char *fmt, ...);
void mesh_log_notice(struct mesh_ctx *ctx, const char *fmt, ...);
void mesh_log_warn(struct mesh_ctx *ctx, const char *fmt, ...);
void mesh_log_err(struct mesh_ctx *ctx, const char *fmt, ...);

int mesh_is_hex(const char *s);
int mesh_str_to_hex(const char *in, char *out, size_t outlen);
int mesh_hex_to_str(const char *in, char *out, size_t outlen);
int mesh_is_ipv4addr_valid(const char *s, char *canon, size_t canonlen);

int mesh_get_factory_mac(struct mesh_ctx *ctx, char *mac_out, size_t mac_len);

/* wireless helpers */
int mesh_get_mesh_iflist(struct mesh_ctx *ctx);
int mesh_wait_for_interface(struct mesh_ctx *ctx, const char *ifname, char *status_out, size_t status_len);
int mesh_radiophy(const char *radio_or_section, char *phy_out, size_t phy_len);
int mesh_get_wiphys(struct mesh_ctx *ctx);
int mesh_get_valid_channels(char *out, size_t outlen);
int mesh_is_channel_valid(const char *band, const char *channel);
int mesh_refresh_bridgemac(struct mesh_ctx *ctx);
int mesh_sys_setup(struct mesh_ctx *ctx);
int mesh_set_txpower_uci(struct mesh_ctx *ctx);
int mesh_cli_txpower(struct mesh_ctx *ctx, const char *arg);
int mesh_cli_stations(struct mesh_ctx *ctx);
int mesh_cli_country(struct mesh_ctx *ctx, const char *code);
int mesh_cli_mesh_rssi_threshold(struct mesh_ctx *ctx, const char *dir, int force);

/* auto_config / backups */
int mesh_backup_defaults(struct mesh_ctx *ctx);
int mesh_auto_config(struct mesh_ctx *ctx);
int mesh_mesh_gate_state(struct mesh_ctx *ctx);
int mesh_restart_mesh(struct mesh_ctx *ctx);
int mesh_apply_mesh_params(struct mesh_ctx *ctx);

/* portal / peer */
int mesh_get_portal_state(struct mesh_ctx *ctx);
int mesh_check_portal(struct mesh_ctx *ctx);
int mesh_portal_watchdog(struct mesh_ctx *ctx);
int mesh_set_ula_prefix(struct mesh_ctx *ctx, const char *mode);
int mesh_get_portal_ula(struct mesh_ctx *ctx, const char *mode);
int mesh_get_portal_type(struct mesh_ctx *ctx);
int mesh_restore_ipv4(struct mesh_ctx *ctx);
int mesh_ccpy_setup(struct mesh_ctx *ctx);
int mesh_get_peers(struct mesh_ctx *ctx);

/* bridge / vxlan / STP */
int mesh_convert_to_la(const char *mac, char *out, size_t outlen);
int mesh_get_indexed_mac(struct mesh_ctx *ctx, const char *devname, char *out, size_t outlen);
int mesh_set_bridge_mac_addresses(struct mesh_ctx *ctx);
int mesh_set_root_bridge_priority(struct mesh_ctx *ctx);
int mesh_check_vtunnel(struct mesh_ctx *ctx);
int mesh_create_vxlan_if(struct mesh_ctx *ctx);
int mesh_block_bridge_loops(struct mesh_ctx *ctx);

int mesh_cli_dispatch(struct mesh_ctx *ctx, int argc, char **argv);
int mesh_daemon_run(struct mesh_ctx *ctx);

#endif
