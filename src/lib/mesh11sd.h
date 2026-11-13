/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH11SD_H
#define MESH11SD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef MESH11SD_VERSION
#define MESH11SD_VERSION "8.0.0_beta1"
#endif

#define MESH11SD_TMPDIR "/tmp/mesh11sd"
#define MESH11SD_CONFIG "mesh11sd"
#define MESH11SD_LOG_NAME "mesh11sd_log.log"
#define MESH11SD_MAX_IFACES 16
#define MESH11SD_MAX_RADIOS 8
#define MESH11SD_MAX_PEERS 128

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
	int mesh_metric_threshold;
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
	int channel_bandwidth_2g;	/* 20 or 40 MHz */
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
	char last_role[16];	/* mesh11sd.setup.last_role: role at last boot */
	char is_portal[32];
	char portal_ula[64];
	char ula_prefix[48];
	char ula_base[32];
	char ula_subnet[8];
	char ula_vxsubnet[8];
	char portal_type[16];
	int link_to_portal;
	/* Main-loop pass counter, and the pass in which portal discovery
	 * last failed, so a dead portal is probed once per pass. */
	unsigned int loop_seq;
	unsigned int portal_fail_seq;
	int portal_metric;
	int portal_hops;
	char portal_mac[32];
	char up_zone[64];
	char default_ipv6_gw[64];
	char default_ipv4_gw[64];
	int failcount;
	int network_restart;
	int firstloop;
	int changed;
	int macfixup;
	int daemon_startup;
	int leechmode;
	int reboot_on_error;
	int stop_on_error;
	int manage_opennds_startup;
	int swconfig_mac_skip;

	char mesh_backhaul_led[64];
	char mesh_driver[32];
	int mesh_mac_forced_forwarding;
	int gateway_proxy_arp;
	int watchdog_nonvolatile_log;
	int mesh_path_stabilisation;
	char vtun_ip[32];
	char vtun_mask[16];
	int vtun_path_cost;
	int vtun_isolate;
	int move_ethernet_to_vxlan_bridge;
	int connect_show_all;
	char vxlan_group[64]; /* override for create_vxlan_if; empty → ff02::$tun_id */
	char cpe_mode[16];

	char peerlist[MESH11SD_MAX_PEERS][32];
	int peerlist_count;
	char iflist[MESH11SD_MAX_IFACES][32];
	int iflist_count;
	char radios[MESH11SD_MAX_RADIOS][32];
	int radio_count;
	char wiphys[MESH11SD_MAX_RADIOS][16];
	int wiphy_count;
};

/* util */
int mesh_run_capture(char *const argv[], char *out, size_t outlen);
int mesh_sysfs_read_int(const char *path, int *out);
int mesh_sysfs_write_int_if_changed(const char *path, int want); /* 0=same, 1=wrote, -1=err */
int mesh_sha256_hex(const char *in, char *out, size_t outlen);
int mesh_sha256_hex_echo(const char *in, char *out, size_t outlen); /* v7 echo|sha256sum */
int mesh_is_installed(const char *package);
int mesh_check_package_any(const char *const *names, int *installed_flags);
int mesh_check_requirements(struct mesh_ctx *ctx); /* 0=ok, -1=unmet; logs on change */
int mesh_file_write(const char *path, const char *content);
int mesh_mkdir_p(const char *path);
/* ignore_ip: current network.*.ipaddr (no CIDR) so re-entry is idempotent —
 * v7 only generates once at autostartup; the port calls apply from several
 * sites and must not treat the address we are replacing as "used elsewhere". */
int mesh_generate_ipv4_base(struct mesh_ctx *ctx, const char *seed, char *out,
			    size_t outlen, const char *ignore_ip);
int mesh_apply_portal_ipv4(struct mesh_ctx *ctx); /* v7 autostartup portal_use_default_ipv4=0 */
void mesh_reserve_bridge_macs(struct mesh_ctx *ctx);
int mesh_fixup_wireless_macaddrs(struct mesh_ctx *ctx); /* v7 ~11225 macaddr UCI before wifi */

/* uci_ops: v7 uncommitted default; commit_all mode honoured by callers */
int mesh_uci_get(const char *tuple, char *buf, size_t buflen);
int mesh_uci_set(const char *tuple, const char *value);
int mesh_uci_add(const char *tuple); /* uci add ... */
int mesh_uci_commit(const char *package);
int mesh_uci_revert(const char *package);
int mesh_uci_changes(const char *package, char *buf, size_t buflen);
void mesh_uci_restage_change(const char *line);
int mesh_uci_commit_option(const char *package, const char *tuple, const char *value);
void mesh_network_mark_applied(void);
int mesh_network_reload_if_changed(struct mesh_ctx *ctx, const char *who);
int mesh_uci_batch_line(const char *line); /* "set foo.bar='baz'" */
int mesh_uci_show_filter(const char *package, const char *grep_pat,
			 char *buf, size_t buflen);

void mesh_ctx_init(struct mesh_ctx *ctx, int is_daemon);
int mesh_ctx_load_setup(struct mesh_ctx *ctx);
int mesh_check_config_params(struct mesh_ctx *ctx);
void mesh_manage_opennds(struct mesh_ctx *ctx, const char *action);

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
int mesh_wait_for_wifi_enabled(struct mesh_ctx *ctx); /* after wifi: hostapd/wpa ENABLED */
int mesh_iface_is_up(const char *ifname); /* non-blocking; 1=up/unknown, 0=down/absent */
int mesh_ensure_iface_arp_multicast(struct mesh_ctx *ctx, const char *ifname);
int mesh_radiophy(const char *radio_or_section, char *phy_out, size_t phy_len);
int mesh_phy_is_multi_band(const char *phy); /* v7 MLO wiphy radios section */
int mesh_phy_info_has(const char *phy, const char *needle); /* stream iw phy info */
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
int mesh_backup_configs(struct mesh_ctx *ctx);
int mesh_auto_config(struct mesh_ctx *ctx);
int mesh_mesh_gate_state(struct mesh_ctx *ctx);
int mesh_restart_mesh(struct mesh_ctx *ctx);
int mesh_all_nodes_rssi_update(struct mesh_ctx *ctx);
int mesh_apply_mesh_params(struct mesh_ctx *ctx);

/* portal / peer */
int mesh_get_portal_state(struct mesh_ctx *ctx);
int mesh_wan_upstream_present(struct mesh_ctx *ctx);
int mesh_portal_probe(const char *addr);
int mesh_check_portal(struct mesh_ctx *ctx);
int mesh_ensure_portal_dhcp_force(struct mesh_ctx *ctx);
int mesh_ensure_portal_vtun_dnsmasq(struct mesh_ctx *ctx);
int mesh_portal_watchdog(struct mesh_ctx *ctx);
void mesh_peer_reapply(struct mesh_ctx *ctx);   /* peer overlay again */
void mesh_peer_stage_uci(struct mesh_ctx *ctx);  /* peer network/dhcp values */
int mesh_predicted_peer(struct mesh_ctx *ctx);   /* last_role peer, mode 1/4 */
void mesh_portal_reapply(struct mesh_ctx *ctx); /* portal runtime again */
int mesh_set_ula_prefix(struct mesh_ctx *ctx, const char *mode);
int mesh_get_portal_ula(struct mesh_ctx *ctx, const char *mode);
/* Split fdxx:xxxx:xxxx:subnet::1 into base (first 3 hextets) and subnet. */
int mesh_ula_parts_from_addr(const char *addr, char *base, size_t baselen,
			     char *subnet, size_t subnetlen);
int mesh_manage_fallback_ula_route(struct mesh_ctx *ctx); /* v7 ULA ::1 + EUI64 */
int mesh_get_portal_type(struct mesh_ctx *ctx);
int mesh_restore_ipv4(struct mesh_ctx *ctx);
int mesh_check_odhcpd(struct mesh_ctx *ctx);
int mesh_ccpy_setup(struct mesh_ctx *ctx);
int mesh_get_peers(struct mesh_ctx *ctx);
int mesh_detect_mbp_upstream(struct mesh_ctx *ctx);
int mesh_detect_cpe_upstream(struct mesh_ctx *ctx);
/* cpe.c: portal_detect 3 */
int mesh_cpe_stage_network(struct mesh_ctx *ctx);
int mesh_cpe_conf(struct mesh_ctx *ctx);
int mesh_cpe_discovery_rule(struct mesh_ctx *ctx);
int mesh_detect_l3_upstream(struct mesh_ctx *ctx);
int mesh_update_known_portals_from_ras(struct mesh_ctx *ctx);
int mesh_calc_airtime_metric(struct mesh_ctx *ctx, const char *peer_mac,
			     int *metric_out, int *hops_out);
int mesh_get_metrics_from_ula(struct mesh_ctx *ctx, const char *ula,
			      int *metric_out, int *hops_out, char *peer_mac, size_t peer_len);
int mesh_portal_ula_discover(struct mesh_ctx *ctx);

/* bridge / vxlan / STP */
int mesh_convert_to_la(const char *mac, char *out, size_t outlen);
int mesh_convert_from_la(const char *mac, char *out, size_t outlen);
int mesh_get_indexed_mac(struct mesh_ctx *ctx, const char *devname, char *out, size_t outlen);
int mesh_set_bridge_mac_addresses(struct mesh_ctx *ctx);
int mesh_set_root_bridge_priority(struct mesh_ctx *ctx);
int mesh_check_vtunnel(struct mesh_ctx *ctx);
int mesh_vtunnel_preconfigure(struct mesh_ctx *ctx);
int mesh_network_preconfigure(struct mesh_ctx *ctx);
int mesh_create_vxlan_if(struct mesh_ctx *ctx);
int mesh_update_vxlan_for_active_portal(struct mesh_ctx *ctx);
int mesh_block_bridge_loops(struct mesh_ctx *ctx);
int mesh_check_mesh_phantom(struct mesh_ctx *ctx); /* v7 phantom mpath del */
int mesh_pin_active_portal_arp(struct mesh_ctx *ctx); /* v7 ARP pin */
int mesh_pin_active_portal_arp_clear(struct mesh_ctx *ctx);
int mesh_move_ethernet_to_vxlan_bridge(struct mesh_ctx *ctx);
int mesh_ensure_wan_on_vxlan_bridge(struct mesh_ctx *ctx, int do_reload); /* modes 4|5 */
void mesh_check_accept_ra(struct mesh_ctx *ctx); /* v7 per-pass accept_ra */
int mesh_probe_interface_mac(struct mesh_ctx *ctx);
int mesh_set_iface_path_cost(struct mesh_ctx *ctx, int path_cost,
			     const char *bridge, const char *iface);

/* mobility / HWMP / channel / gate */
int mesh_mobility_settings(struct mesh_ctx *ctx, const char *mode);
int mesh_mobility_phywide_differs(struct mesh_ctx *ctx);
int mesh_mobility_apply_phywide(struct mesh_ctx *ctx); /* 1 if a value was written */
int mesh_phy_rts_display(const char *phy, char *out, size_t outlen); /* live or unsupported */
int mesh_rootmode(struct mesh_ctx *ctx);
int mesh_check_path_changes(struct mesh_ctx *ctx);
int mesh_path_lookup_lifetime(const char *mac, long *lifetime_ms);
int mesh_check_channel(struct mesh_ctx *ctx);
int mesh_check_gate(struct mesh_ctx *ctx);
int mesh_scan_channel(struct mesh_ctx *ctx);
int mesh_fix_mesh_noscan(struct mesh_ctx *ctx);

/* remote CLI ops */
int mesh_cli_connect(struct mesh_ctx *ctx, int argc, char **argv);
int mesh_cli_copy(struct mesh_ctx *ctx, int argc, char **argv);
int mesh_cli_mesh_leechmode(struct mesh_ctx *ctx, const char *arg);
int mesh_cli_mesh_node_mobility_level(struct mesh_ctx *ctx, const char *arg);
int mesh_cli_force_ipv4_download(struct mesh_ctx *ctx);
int mesh_cli_download_revert_to_default(struct mesh_ctx *ctx);
int mesh_cli_dhcp4_renew(struct mesh_ctx *ctx, const char *dev);
int mesh_cli_is_installed(struct mesh_ctx *ctx, const char *pkg);

/* commit / revert */
int mesh_cli_commit_changes(struct mesh_ctx *ctx, int argc, char **argv);
int mesh_cli_commit_all(struct mesh_ctx *ctx, int argc, char **argv);
int mesh_cli_revert_all(struct mesh_ctx *ctx, int argc, char **argv);
int mesh_cli_restore_all(struct mesh_ctx *ctx, int argc, char **argv);
int mesh_maybe_silent_commit_all(struct mesh_ctx *ctx);
int mesh_commit_portal_config(struct mesh_ctx *ctx); /* auto_config=1, before detection */

/* managed: keep LuCI usable (docs/LUCI-DESIGN.md) */
int mesh_managed_mode(struct mesh_ctx *ctx);
int mesh_managed_pass(struct mesh_ctx *ctx, int auto_config_done);
int mesh_write_portal_copies(struct mesh_ctx *ctx, const char *const *pkgs);
void mesh_apply_autostartup_uci(struct mesh_ctx *ctx);

/* MAC addresses set by mesh11sd's MAC walk and fixup (never committed) */
#define MESH_OWN_MACS_MAX 32
struct mesh_own_mac {
	char sect[64];
	char val[32];
};
int mesh_own_macs_find(struct mesh_ctx *ctx, const char *conf,
		       struct mesh_own_mac *tab, int max);
void mesh_own_macs_strip(const char *conf, const struct mesh_own_mac *tab, int n);
void mesh_own_macs_set(const char *conf, const struct mesh_own_mac *tab, int n);
void mesh_clear_own_macaddrs(struct mesh_ctx *ctx);

/* apmond-style JSON (Spod) — reuse for status etc. */
void mesh_json_str(const char *in, char *out, size_t outlen);
void mesh_json_field(const char *indent, const char *key, const char *value, int last);
void mesh_json_field_int(const char *indent, const char *key, long value, int last);

int mesh_cli_status(struct mesh_ctx *ctx);
int mesh_set_led(struct mesh_ctx *ctx, const char *action);
int mesh_check_mesh_active_led(struct mesh_ctx *ctx);
int mesh_get_node_type_code(struct mesh_ctx *ctx, char *out, size_t outlen);
int mesh_get_mesh_params_dump(const char *iface, char *out, size_t outlen);

int mesh_cli_dispatch(struct mesh_ctx *ctx, int argc, char **argv);
int mesh_daemon_run(struct mesh_ctx *ctx);

#endif
