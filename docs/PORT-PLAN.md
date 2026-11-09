# mesh11sd v8 Stage 1 port plan

Base: v7.3.2 @ eb98726 (`src/mesh11sd`, ash). Target: readable C, same CLI names/output, same OpenWrt paths (`/etc/config/mesh11sd`, `/etc/init.d/mesh11sd`).

UCI policy (Stage 1, faithful to v7):
- Default: uncommitted (`setup.commit_all` unset/0). Role and auto_config writes use `uci set`/`uci batch` without commit; stop reverts packages.
- Committed mode: `setup.commit_all=1`, or `auto_config=2` (runs `commit_all` once mesh is active). Backups at `/etc/config/<pkg>_mesh11sd_default`.
- All UCI I/O goes through `src/lib/uci_ops.*` so both modes stay one place. LuCI review is after the port runs (Stage 2 topic).

## CLI inventory (v7)

Public (help): help/version, debuglevel, enable, disable, auto_config, status, connect, copy, txpower, stations, mesh_rssi_threshold, mesh_leechmode, mesh_node_mobility_level, commit_changes, commit_all, revert_all, force_ipv4_download, download_revert_to_default, dhcp4_renew, is_installed, get_portal_ula, set_ula_prefix, str_to_hex, hex_to_str, is_hex, is_ipv4addr_valid, country, write_to_syslog, write_log, read_log, get_valid_channels.

Extra (no help, used internally/tests): get_indexed_mac, get_factory_mac, wifi_chipset_detect, active_nodecount, get_node_type_code, get_wiphys, get_wiphy_by_ifname, probe_interface_mac, radiophy, is_channel_valid, update_known_portals_from_ras, get_metrics_from_ula/mac, calc_airtime_metric, get_ethernet_clients, get_eth_stats, remove_last_char, get_portal_type, daemon.

Out of Stage 1 (do not port): apmond, write_node_data, send_ap_data, get_ap_data, show_ap_data, luci cgi redirect/restore for apmon. Dropped from chunk 10.

## Daemon loop (v7 line ranges)

| Phase | Lines (approx) | Notes |
|-------|----------------|-------|
| Startup / get_current_setup | 29–1270, 9066 | Read options, tmpdir, checksum |
| check_config_params | 1271–1429 | Default mesh_params via uncommitted set |
| sys_setup | 4207–4247 | sysctl arp/ndp |
| Main loop entry | 11165–11600 | while true |
| auto_config / ccpy_setup | 3780–4132, 3668–3766 | Wireless/network/dhcp/firewall |
| Portal detect / check_portal / watchdog | 2662–3470, 2895–3062 | MRP/MBP/CPE/TPN |
| check_gate / check_channel | 1668–2044 | Mesh gate + channel track |
| Bridge MAC / STP / vxlan | 8157–8347, 5055–5550 | set_bridge_mac, vtunnel |
| Path / mobility / HWMP | 4259–4480, 4469–4605 | mobility_settings |
| block_bridge_loops / odhcpd | 4606–5283 | nft + dhcp |
| loop_sleep_interval | 1715–1765 | load-based pause |

## Port chunks (order toward installable alpha)

1. **Scaffold + simple CLI** (done): dispatcher, version/help, debuglevel, enable, disable, minimal status, get_factory_mac, write_to_syslog/write_log/read_log, is_hex/str_to_hex/hex_to_str/is_ipv4addr_valid, uci_ops (uncommitted default; commit API stubs), init + default config, CMake/OpenWrt Makefile, stub `daemon` (no mesh apply).
2. **Config load + daemon skeleton** (done): full get_current_setup option read, checkinterval loop, enabled gate, sys_setup, logging parity, procd respawn behaviour.
3. **Wireless/radio helpers** (done): get_mesh_iflist, radiophy, get_wiphys, wifi_chipset_detect, get_valid_channels, is_channel_valid, country CLI, txpower/stations/mesh_rssi_threshold (live iw).
4. **auto_config** (done — core auto_config + backups; ccpy peer scan in chunk 5): mesh/AP iface creation, backups `_mesh11sd_default`, encryption/OWE, mesh_params apply; honour `commit_all` when set.
5. **Portal detect + ccpy peer scan** (done): get_portal_state/type, check_portal, portal_watchdog, ULA, peer/portal handoff with `network.<zone>.proto='dhcp'` (not none), ccpy_setup + ndp_scan_peer for join.
6. **Bridge/vxlan/STP**: set_bridge_mac_addresses (LA indexed), vtunnel/vxlan, block_bridge_loops (nft mesh_PRE), root bridge STP priority.
7. **Mobility + channel track**: mobility_settings, check_channel, check_gate, HWMP rootmode, path changes.
8. **CLI remote/ops**: connect, copy, mesh_leechmode, mesh_node_mobility_level, force_ipv4_download, download_revert_to_default, dhcp4_renew, is_installed, set_ula_prefix, get_portal_ula.
9. **commit_changes / commit_all / revert_all**: selective commit of runtime mesh11sd changes; full commit of mesh11sd/wireless/dhcp/network/firewall/system; restore from `_mesh11sd_default` + reboot; `auto_config=2` silent commit_all. Both UCI modes complete here.
10. **Status JSON + LEDs**: full status JSON output, LED helpers. No apmond / ap_data / cgi.

Shelved (reference only): branches `8.0.0alpha`, `8.0.0alpha-uci-handoff`; `/workspace/mesh11sd-testbed/*`. May copy proven pieces (factory MAC chain, wpad check, PHY) into later chunks. Explicitly not porting apmond.
