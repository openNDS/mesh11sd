# UCI ownership (LuCI design audit)

Read-only comparison of what is settable in the mesh11sd UCI config
(`setup` and `mesh_params`) against every option v8 writes in
`wireless`, `network`, `firewall`, `dhcp` and `system`.

Source: branch `8.0.0beta` at `da02396`, default config in
`openwrt/mesh11sd/files/etc/config/mesh11sd` and README §12, parsing
in `src/lib/config.c`, writes via `mesh_uci_set` / `mesh_uci_batch_line`
in `src/lib` and `src/mesh11sd`. No commits.

**Intent:** settings that matter to mesh11sd belong in its config.
Edits made in LuCI (or elsewhere) to those options should be reverted
from the mesh11sd copy. Unrelated user settings should survive.

`$net` below is `mesh11sd.setup.auto_mesh_network` (default `lan`).
`m11sN` and `vxradioN` are the auto-created wifi-iface sections.
Values in **bold** in the gaps list are mesh-critical and have no
mesh11sd option today.

---

## A. Options already in mesh11sd

### `setup` — parsed by `mesh_ctx_load_setup`

Core: `enabled`, `debuglevel`, `checkinterval`, `loop_interval_delta`,
`interface_timeout`, `auto_config`, `commit_all` (set by `commit_all`,
not a user-facing knob), `portal_detect`, `portal_detect_threshold`,
`portal_use_default_ipv4`, `portal_channel` (2.4 GHz only),
`channel_tracking_checkinterval`.

Identity and radio: `auto_mesh_id`, `auto_mesh_key`, `auto_mesh_band`
(`2g`, `2g40`, `5g`, `6g`, `60g`), `mesh_phy_index`,
`mesh_multi_interfaces`, `country`, `mesh_basename`,
`auto_mesh_network`, `txpower`.

Access points: `mesh_gate_enable`, `mesh_gate_base_ssid`,
`mesh_gate_encryption`, `mesh_gate_key`, `ssid_suffix_enable`,
`mesh_leechmode_enable`.

VXLAN: `vtun_enable`, `tun_id`, `vtun_ip`, `vtun_mask`,
`vtun_base_ssid`, `vtun_gate_encryption`, `vtun_gate_key`,
`vtun_path_cost`, `vtun_isolate`, `move_ethernet_to_vxlan_bridge`
(runtime `brctl` only; no UCI write).

Path, watchdog, CPE: `mesh_path_cost`, `mesh_path_stabilisation`,
`mesh_mac_forced_forwarding`, `swconfig_mac_skip`, `gateway_proxy_arp`,
`mesh_metric_threshold`, `reboot_on_error`, `stop_on_error`,
`watchdog_nonvolatile_log`, `mesh_backhaul_led`, `log_mountpoint`,
`max_log_entries`, `use_default_beacon_interval`, `mesh_dtim_period`,
`mesh_node_mobility_level`, `cpe_mode` (parsed, **not yet applied** to
dhcp/network/firewall; code default is `auto`, docs say `nat66`),
`odhcpd_log_level`.

### `setup` — documented but not parsed in v8

`manage_opennds_startup`, `wpad_debuglevel`, `apmond_enable`,
`apmond_cgi_dir`, `apmon_verbose_debug_enable`, `connect_show_all`.
(apmond is now a separate package; these can wait.)
README §7 still lists `reactive_path_stabilisation_threshold`, which
was removed.

### `mesh_params`

Any driver-supported 802.11s parameter may be set here and is reapplied
every `checkinterval`. Startup writes defaults if unset:
`mesh_fwding`, `mesh_retry_timeout`, `mesh_confirm_timeout`,
`mesh_holding_timeout`, `mesh_rssi_threshold`, `mesh_gate_announcements`,
`mesh_hwmp_rootmode`, `mesh_hwmp_root_interval`,
`mesh_hwmp_active_path_to_root_timeout`, `mesh_max_peer_links`,
`mesh_plink_timeout`. Mobility overwrites several HWMP timers, rootmode,
`mesh_fwding`, `mesh_max_peer_links` and sometimes RSSI. Runtime flags:
`mesh_connected_to_gate`, `mesh_connected_to_as`.

These stay in `mesh_params`; they are not copied into wireless/network.

---

## B. Options v8 writes outside mesh11sd

### wireless

| Option | What v8 writes | Value source | mesh11sd option | Recommendation |
| --- | --- | --- | --- | --- |
| `wifi-device.disabled` | `0` on every radio | hardcoded | none (auto_config) | managed (already in mesh11sd) — auto_config always enables every radio |
| `wifi-device.country` | ISO code, or `AD` (DFS-ETSI) if both unset | `country`, else existing wireless, else `AD` | `country` | managed (already in mesh11sd) |
| `wifi-device.channel` (2.4 GHz) | set if not `default` | `portal_channel`: number, or `auto` (hash of device/factory MAC → 1–13), or leave wireless | `portal_channel` | managed (already in mesh11sd). Optional alias: `channel_2g` |
| `wifi-device.channel` (5 / 6 GHz) | **not written** | existing wireless / driver default. Peers follow the portal on air (`iw scan`), not via UCI | **none** | **move to mesh11sd (new override option, suggested name: `channel_5g`, `channel_6g`)** |
| `wifi-device.htmode` / `noscan` (2.4 GHz) | htmode 20→40 and `noscan=1` | `auto_mesh_band=2g40` and `country` set | `auto_mesh_band` | managed (already in mesh11sd). Optional first-class `channel_bandwidth_2g` (`20`/`40`) |
| `wifi-device.htmode` (5 / 6 GHz) | **not written** | existing wireless | **none** | **move to mesh11sd (new override option, suggested name: `channel_bandwidth_5g`, `channel_bandwidth_6g`)** — e.g. 20/40/80/160/320 |
| `wifi-device.txpower` | set only if wireless value is unset | `txpower` | `txpower` | managed (already in mesh11sd) — should overwrite when the option is set, not only when wireless is empty |
| `wifi-device.log_level` | `3` | hardcoded | none (`wpad_debuglevel` is documented, not parsed) | leave to user (or later tie to `wpad_debuglevel`) |
| `wifi-device.beacon_int` | set on the mesh radio only; deleted at mobility 0 | `mesh_node_mobility_level`, forced 100 if `use_default_beacon_interval` or ath11k / mt76_wmac / multi-band wiphy | `mesh_node_mobility_level`, `use_default_beacon_interval` | managed (already in mesh11sd) |
| `m11sN` (section, `device`, `mode=mesh`, `ifname`, `network`) | created by auto_config | `mesh_basename`, `auto_mesh_network`, radio index | those options | managed (already in mesh11sd) |
| `m11sN.mesh_id` | hashed id | SHA-256 of `auto_mesh_id` (default `--__`) | `auto_mesh_id` | managed (already in mesh11sd) |
| `m11sN.key` | hashed key | SHA-256 of mesh id + `auto_mesh_key` (or a built-in suffix) | `auto_mesh_key` | managed (already in mesh11sd) |
| `m11sN.encryption` | `sae` | **hardcoded** | **none** | **move to mesh11sd (new override option, suggested name: `mesh_encryption`)** |
| `m11sN.disabled` | 0 on the chosen band, else 1 | `auto_mesh_band`, `mesh_phy_index`, `mesh_multi_interfaces` | those options | managed (already in mesh11sd) |
| `m11sN.mesh_rssi_threshold` | integer dBm | `mesh_params.mesh_rssi_threshold` (default −63; mobility may overwrite) | `mesh_params.mesh_rssi_threshold` | managed (already in mesh11sd) |
| `m11sN.dtim_period` | integer | `mesh_dtim_period`, then mobility | `mesh_dtim_period`, `mesh_node_mobility_level` | managed (already in mesh11sd) |
| `m11sN.macaddr` | factory MAC if unset and `commit_all=0` | factory / label MAC | none | leave to user once set (internal default only) |
| `wifi-iface` AP `disabled` | 0/1, or 1 on the mesh radio when mode 2 | `mesh_gate_enable` | `mesh_gate_enable` | managed (already in mesh11sd) |
| `wifi-iface` AP `ssid` | base, optional `-<band>-<mac4>-rNpM` suffix | `mesh_gate_base_ssid` or `vtun_base_ssid`; suffix from factory MAC if `ssid_suffix_enable` | `mesh_gate_base_ssid`, `vtun_base_ssid`, `ssid_suffix_enable` | managed (already in mesh11sd). Optional custom suffix: `ssid_suffix` |
| `wifi-iface` AP `encryption` / `key` (LAN APs) | sae / sae-mixed+aes / psk2+aes / owe / none | `mesh_gate_encryption`, `mesh_gate_key` | those options | managed (already in mesh11sd) |
| `wifi-iface` AP `encryption` / `key` (vxradio) | same codes | `vtun_gate_encryption`, `vtun_gate_key` (section first created with `none`, then overwritten) | those options | managed (already in mesh11sd) |
| `wifi-iface` AP `ifname` | `phyN-apI` if unset | generated | none | leave to user once set |
| `wifi-iface` AP `macaddr` | indexed locally-administered MAC if unset and `commit_all=0` | derived from factory MAC | none | leave to user once set |
| `vxradioN` section (`device`, `mode=ap`, `network=vtunlan`, `ifname`, `disabled`) | created when `vtun_enable` | `vtun_enable`, `mesh_gate_enable` | those options | managed (already in mesh11sd) |

Note: if `mesh_gate_base_ssid` is unset, v8 always uses `MeshGate` and
overwrites every non-vtun AP SSID. The documented “use the wireless
SSID if it is not OpenWrt” fallback is not in the C port.

### network

| Option | What v8 writes | Value source | mesh11sd option | Recommendation |
| --- | --- | --- | --- | --- |
| `$net.ipaddr` | portal / LAN IPv4 | hash of `auto_mesh_id` (CPE: factory MAC), skipped if `portal_use_default_ipv4=1` | `portal_use_default_ipv4` only (keep-or-hash; no explicit address) | **move to mesh11sd (new override option, suggested name: `ipv4_address`)** |
| `$net.netmask` | `255.255.255.0` with the hash address | hardcoded | **none** | **move to mesh11sd (new override option, suggested name: `ipv4_mask`)** |
| `$net.proto` | `static` (portal / modes 0,1) or `dhcp` (peer, mode 5) | role / `portal_detect` | `portal_detect` | managed (already in mesh11sd) |
| `$net.ip6assign` | `64` on portal; deleted on peer | role | `portal_detect` | managed (already in mesh11sd) |
| `$net.ip6hint` | 4 hex digits | SHA-256 of factory MAC | none (derived) | managed (already in mesh11sd) — derived; optional override `ula_prefix` if a stable IPv6 prefix must be set by hand |
| `$net.macaddr` | indexed MAC if it differs | derived | `swconfig_mac_skip` (swconfig only) | leave to user once set |
| `globals.ula_prefix` | `fdXX:XXXX:XXXX::/48` | slices of hashed `auto_mesh_id`; revert path restores the backup | `auto_mesh_id` (derived) | managed (already in mesh11sd). Optional same `ula_prefix` override as above |
| `wan6.reqprefix` / `reqaddress` | `auto` / `try` | hardcoded, written with the IPv4 apply | none | leave to user — stop writing unless CPE prefix delegation needs it |
| `vtunnel` device (`name`, `type=bridge`, `bridge_empty`, `ports`) | created with `vtun_enable` | `tun_id` | `vtun_enable`, `tun_id` | managed (already in mesh11sd) |
| `vtunlan` interface (`device`, `proto`, `ipaddr`, `netmask`, `ip6assign`, `ip6hint`, `ip6privacy`) | created with `vtun_enable` | `vtun_ip` (or hash of `auto_mesh_key`), `vtun_mask`, role for proto, factory-hash hint | `vtun_ip`, `vtun_mask`, `tun_id`, `vtun_enable` | managed (already in mesh11sd) |

Mode 4/5 WAN-on-tunnel and `move_ethernet_to_vxlan_bridge` are runtime
`brctl` / `ip` moves, not UCI.

### firewall

| Option | What v8 writes | Value source | mesh11sd option | Recommendation |
| --- | --- | --- | --- | --- |
| `vtunlan` zone (`name`, `network`, `input`/`output`/`forward=ACCEPT`) | created with the tunnel | hardcoded | `vtun_enable` | managed (already in mesh11sd) |
| `vxlan` rule (UDP 4789 from lan) | created with the tunnel | hardcoded | `vtun_enable` | managed (already in mesh11sd) |
| `dhcptun$tun_id` / `dnstun$tun_id` rules | DHCP renew and DNS from vtunlan | hardcoded | `vtun_enable`, `tun_id` | managed (already in mesh11sd) |
| `vxlan_fwd` forwarding vtunlan→wan | created with the tunnel | hardcoded | `vtun_enable` | managed (already in mesh11sd) |
| `lan_tun` forwarding vtunlan→lan | created only if isolation is off | `vtun_isolate=0` | `vtun_isolate` | managed (already in mesh11sd) |

v8 does not write `firewall.lan` or `firewall.wan`. User rules and
zone tweaks outside the vtunlan/vxlan set should be left alone.
CPE (`portal_detect=3`) does not yet get NAT66 / prefix-delegation /
relay rules from `cpe_mode`.

### dhcp

| Option | What v8 writes | Value source | mesh11sd option | Recommendation |
| --- | --- | --- | --- | --- |
| `odhcpd.loglevel` | 0–7 | `odhcpd_log_level` (default 3) | `odhcpd_log_level` | managed (already in mesh11sd) |
| `@dnsmasq[0].logfacility` / `quietdhcp` | `-` / `1` | hardcoded | none | leave to user |
| `@dnsmasq[0].authoritative` | `1` portal, `0` peer | role | `portal_detect` | managed (already in mesh11sd) |
| `@dnsmasq[0].rebind_protection` | `1` portal, `0` peer | role | `portal_detect` | managed (already in mesh11sd) |
| `@dnsmasq[0].server` | deletes `$net.ipaddr` from the list on portal | current IPv4 | none | managed (already in mesh11sd) — housekeeping with the IPv4 apply |
| `$net.dhcpv4` / `dhcpv6` / `ra` / `ignore` / `force` / `ra_default` / `ra_flags` | server+force+managed/other-config on portal; disabled+ignore+none on peer; mode 5 same as peer | role / `portal_detect` | `portal_detect` | managed (already in mesh11sd) |
| `lan.ra_lifetime` / `ra_mininterval` / `ra_maxinterval` | `3600` / `20` / `60` | hardcoded (always `dhcp.lan`, even if `$net` is not `lan`) | none | managed (already in mesh11sd) — operational default; no new option |
| `vtunlan` dhcp section (`interface`, `start=100`, `limit=150`, `leasetime=12h`, `ra_slaac`, `ra_default`, `ra_*`, `ignore`/`force`, flags) | created with the tunnel; server on routed portal, disabled on peer | hardcoded pool; role for enable | `vtun_enable` | managed (already in mesh11sd). DHCP pool itself: leave to user if a later override is wanted |

### system

| Option | What v8 writes | Value source | mesh11sd option | Recommendation |
| --- | --- | --- | --- | --- |
| `@system[0].hostname` | `meshnode-XXXX` (last 4 hex digits of factory MAC) | derived, written every auto_config | none | leave to user — stop overwriting after the first set (timezone, NTP and the rest are already untouched) |

---

## Mesh-critical gaps

These affect joining, addressing or regulatory behaviour and have no
mesh11sd option (or only a keep-or-hash switch):

| Gap | Today | Suggested option |
| --- | --- | --- |
| Backhaul encryption type | always `sae` | `mesh_encryption` (keep SAE as default) |
| Backhaul key / mesh id | already covered | `auto_mesh_key`, `auto_mesh_id` |
| 5 GHz / 6 GHz channel | left to wireless config | `channel_5g`, `channel_6g` (`auto` / `default` / number), same idea as `portal_channel` |
| 2.4 GHz channel | already covered | `portal_channel` (optional alias `channel_2g`) |
| Country | already covered | `country` |
| 5 GHz / 6 GHz channel bandwidth | left to wireless config | `channel_bandwidth_5g`, `channel_bandwidth_6g` |
| 2.4 GHz 20/40 | already covered | `auto_mesh_band` (`2g` / `2g40`) |
| IPv4 address / mask | hash, or keep network config | `ipv4_address`, `ipv4_mask` (when set, override the hash; `portal_use_default_ipv4=1` still means “keep network”) |
| ULA prefix / bridge IPv6 | derived from `auto_mesh_id` + factory MAC | optional `ula_prefix` if a hand-set prefix is needed |
| AP encryption / keys / SSID base / suffix enable | already covered | `mesh_gate_encryption`, `mesh_gate_key`, `vtun_gate_*`, `mesh_gate_base_ssid`, `ssid_suffix_enable` |

---

## Proposed new `setup` options

| Suggested name | Purpose | Default if unset |
| --- | --- | --- |
| `ipv4_address` | Override the hash-derived portal / LAN IPv4 | hash (or network config if `portal_use_default_ipv4=1`) |
| `ipv4_mask` | Override the hardcoded `255.255.255.0` | `255.255.255.0` |
| `channel_5g` | Portal channel on 5 GHz: `auto`, `default`, or a valid channel | `default` (leave wireless) |
| `channel_6g` | Same for 6 GHz (test on the GL-BE10000) | `default` |
| `channel_2g` | Optional alias of existing `portal_channel` | `default` |
| `channel_bandwidth_5g` | 20/40/80/160/320 (as the radio allows) | leave wireless |
| `channel_bandwidth_6g` | same | leave wireless |
| `channel_bandwidth_2g` | Optional 20/40; `auto_mesh_band=2g40` already does this | follow `auto_mesh_band` |
| `mesh_encryption` | Backhaul encryption type (today always SAE) | `sae` |
| `ssid_suffix` | Custom AP id suffix; empty keeps the last 4 factory-MAC digits when `ssid_suffix_enable=1` | MAC-derived |
| `ula_prefix` | Optional hand-set ULA `/48` instead of the hash | derived from `auto_mesh_id` |

Already present for the rest of the wanted list: `country`,
`portal_channel`, `auto_mesh_band` (2.4 GHz 20/40),
`mesh_gate_base_ssid`, `ssid_suffix_enable`, `mesh_gate_encryption`,
`mesh_gate_key`, `auto_mesh_key`, `vtun_base_ssid`,
`vtun_gate_encryption`, `vtun_gate_key`.

---

## Notes for the LuCI revert

- `commit_all` writes `mesh11sd`, `wireless`, `dhcp`, `network`,
  `firewall` and `system`. Selective restore should touch only the
  rows marked managed or moved, not a wholesale copy of those files.
- Role flips (portal ↔ peer) rewrite `$net.proto`, dhcp server/ignore
  and ULA assign at runtime. Those stay managed even when LuCI is left
  enabled; they are not user knobs.
- `auto_config=0` still honours an existing wireless mesh config even
  if it is wrong; this audit is about the auto_config path.
- `cpe_mode` and the documented-but-unparsed options are mesh11sd
  settings, not gaps in wireless/network ownership. CPE IPv6 (NAT66 /
  prefix delegation / relay) still needs its own implementation pass.

## Decisions (Rob, Nov 11 2026)

- `auto_mesh_band` is rationalised to `2g`, `5g` and `6g`. `2g40` is dropped; bandwidth comes from the new `channel_bandwidth_2g`, `channel_bandwidth_5g` and `channel_bandwidth_6g` options.
- `portal_channel` is extended to cover all bands. The channel and bandwidth are both checked against the country code, with sensible defaults; if a check fails, the default is used.
- The hostname `meshnode-xxxx` is set deliberately every time, so an admin always knows which node they are logged into. It also identifies the node in the apmond database.
- The missing documented options (`wpad_debuglevel`, `manage_opennds_startup`, `connect_show_all`) will be ported. The old apmond options will not, because apmond is now a separate package.
- Before release, mesh11sd will send mesh data to apmond over ubus. This is a separate task from the LuCI design.
