## 1. The mesh11sd project

Mesh11sd is an OpenWrt daemon for **IEEE 802.11s** mesh backhaul. It configures radios, portal/peer role, DHCP/RA, optional VXLAN guest/trunk, and access-point monitoring without a central controller.

**New users:** start with [GETTING_STARTED.md](GETTING_STARTED.md).

This documentation applies to version **7.2.x** on **OpenWrt 25.12** and later (apk package manager; this release is not backported to 24.10 or earlier). Setup options are documented in [§7](#7-configuration---setup-options); the commented source UCI file is in [§12](#12-default-configuration-file).

***Read this document (or Getting Started) before installing.***

User devices do not join 802.11s. They join a **mesh gate** (normal AP) on a node. 802.11s is not 802.11r (client roaming).

Mesh11sd was originally built for captive-portal venues and is released under the GNU GPL v2 or later.

## 2. Overview

The daemon runs on every node and treats the mesh as one distributed system. Cabled Ethernet segments are supported and, by default, preferred over wireless (STP path cost). From 6.x, HWMP and related parameters are tuned for node **mobility** (`mesh_node_mobility_level`, default **2**).

### Dependencies (full function)

1. `wpad-mbedtls` (replace `wpad-basic-mbedtls`)
2. `luci-ssl`
3. `luci-app-commands`
4. `ip-full`
5. `kmod-nft-bridge`
6. `vxlan`

If the image uses **ath10k-ct**, switch to non-ct ath10k packages; otherwise the daemon logs an error and exits.

## 3. Features

1. **Auto configuration** of 802.11s mesh backhaul
2. **Mesh Routed Portal (MRP)** — IPv4 NAT and IPv6 routing; optional VXLAN trunk
3. **Auto detect (`portal_detect=1`)** — WAN up → MRP; WAN down → Mesh Peer (MPE)
4. **Mesh Bridge Portal (MBP)** — bridged to the existing LAN/ISP (no IPv4 NAT); WAN is the vxlan trunk
5. **Trunk Peer (TPN)** — WAN is a vxlan trunk endpoint (VLANs); LAN is the mesh (no VLANs)
6. **CPE** — mesh is the WAN; LAN is a private NAT network (IPv4 + NAT66 / PD / relay)
7. **OWE** on mesh gates by default (optional WPA2/WPA3)
8. **Guest / vxtunnel** SSIDs over VXLAN without configuring 802.1Q on the mesh
9. **apmond** — AP and Ethernet client stats collected on the portal (`mesh11sd show_ap_data`)
10. **Mobility levels** 0–6 (level 6 is PTT/voice-oriented)
11. **Cabled backhaul** with STP path cost
12. **OpenNDS** — if installed, mesh11sd starts/stops it as the node becomes portal or peer

## 4. Node types

`portal_detect` is ignored if `auto_config` is 0.

| `portal_detect` | Code | Behaviour |
| --- | --- | --- |
| **1** (default) | MRP / MPE | Auto: WAN link → routed portal (DHCP/RA into the mesh). No WAN → layer-2 peer (DHCP client). |
| **0** | MRP | Always routed portal. |
| **3** | CPE | Mesh is upstream WAN. LAN is a NAT subnet (`cpe_mode`: `nat66` default, `prefix_delegation`, `relay`). |
| **4** | MBP | Bridged portal. **LAN** to ISP/existing LAN. **WAN** is vxlan trunk (`br-tun$tun_id`). |
| **5** | TPN | Peer. **LAN** = mesh (no VLANs). **WAN** = vxlan trunk (VLANs OK). Works with portal 0, 1, or 4. |
| **2** | — | Unused (use 5). |

A node can also be a **gate** (hosts APs). **Leech mode** (`mesh_leechmode_enable`) is an AP that uses the mesh but does not forward; only with mobility level 0 and not on a portal.

IPv6 Unique Local Addresses (ULA) on the mesh are derived from `auto_mesh_id` and the factory MAC. Peers discover the portal ULA from RAs. `mesh11sd connect` lists **label/factory MACs** because the mesh vif is set to that MAC.

## 5. Getting a mesh running

Hardware: enough flash/RAM for the OpenWrt version, preferably WAN+LAN Ethernet, at least one 802.11s radio.

There are two deployment methods. **Firmware Selector / Image Builder is preferred.**

### 5.1 Confidence test

Set `auto_config` to **0** in the image. After flash, SSH in (this first session may still use `192.168.1.1`) and run:

```
mesh11sd auto_config test
exit
```

Do not leave that SSH session open: as the daemon starts it changes the IPv4 subnet and the session will hang. `debuglevel` defaults to 3. Reconnect with IPv6 link-local ([§5.3](#53-accessing-a-node)), then `mesh11sd status`. A power cycle undoes the test. See [GETTING_STARTED.md](GETTING_STARTED.md).

### 5.2 Rapid deployment (Firmware Selector)

Open [firmware-selector.openwrt.org](https://firmware-selector.openwrt.org/), pick model and **OpenWrt 25.12** (or later), then “Customize installed packages and/or first boot script”.

In **Installed Packages**, prefix `wpad-basic-mbedtls` and `luci` with `-`. Append:

```
wpad-mbedtls luci-ssl luci-app-commands ip-full kmod-nft-bridge vxlan mesh11sd
```

Example (GL-MT300N-V2 on OpenWrt 25.12; your base list will differ). The default list includes `apk-mbedtls` instead of `opkg`:

```
base-files busybox ca-bundle dnsmasq dropbear firewall4 fstools kmod-gpio-button-hotplug
kmod-leds-gpio kmod-mt7603 kmod-nft-offload kmod-usb-ohci kmod-usb2 libc libgcc
libustream-mbedtls logd luci mtd netifd nftables odhcp6c odhcpd-ipv6only apk-mbedtls
ppp ppp-mod-pppoe procd procd-seccomp procd-ujail swconfig uci uclient-fetch urandom-seed
urngd -wpad-basic-mbedtls wpad-mbedtls luci-ssl luci-app-commands ip-full kmod-nft-bridge vxlan mesh11sd
```

![Installed packages](https://github.com/openNDS/mesh11sd/blob/master/docs/images/installed-packages.png)

**First boot script** (uci-defaults), confidence test:

```
# auto_config 0 = confidence test. Set to 1 when the hardware is proven.
uci set mesh11sd.setup.auto_config='0'
uci set mesh11sd.setup.auto_mesh_id='MyMeshID'
uci set mesh11sd.setup.mesh_gate_base_ssid='MyNetwork'
uci set mesh11sd.setup.mesh_gate_encryption='1'
uci set mesh11sd.setup.mesh_gate_key='MyWifiCode'
uci commit mesh11sd

rootpassword="myrootpassword"
/bin/passwd root << EOF
$rootpassword
$rootpassword
EOF
```

![uci-defaults](https://github.com/openNDS/mesh11sd/blob/master/docs/images/uci-defaults-highlighted.png)

Use the same `auto_mesh_id` (and `auto_mesh_key` if set) on every hardware type. `mesh_gate_base_ssid` is at most 22 characters if `ssid_suffix_enable=1`. `mesh_gate_encryption=0` is OWE Transition (Enhanced Open, fallback to open).

With `vtun_enable=1` (default except CPE), a **VTunnel** guest SSID is created on the vxlan overlay (OWE by default).

Request the build, flash all nodes of that type. Plug **one** node's **WAN** into the ISP LAN for a default routed portal (`portal_detect=1`). For **MBP**, after the mesh works, set `portal_detect=4` **only on the portal**: LAN to the existing LAN/ISP, WAN is the trunk.

**CPE first-boot snippet:**

```
uci set mesh11sd.setup.auto_config='1'
uci set mesh11sd.setup.portal_detect='3'
uci set mesh11sd.setup.auto_mesh_id='MyMeshID'
uci set mesh11sd.setup.mesh_gate_base_ssid='MyNetwork'
uci set mesh11sd.setup.mesh_gate_encryption='1'
uci set mesh11sd.setup.mesh_gate_key='MyWifiCode'
uci commit mesh11sd
```

### 5.3 Accessing a node

Portal IPv4 is hashed from the label MAC; it is not `192.168.1.1`. Use **IPv6 link-local**.

Note the last two octets of the label MAC, e.g. `94:83:c4:a2:8e:c9` → remember `8ec`. Connect a PC to a LAN port of the node.

**Linux**

Run `ip -6 route`. Look for `via fe80:…` whose last hextet starts with that prefix, e.g. `fe80::9683:c4ff:fea2:8ecb` on interface `enp3s0f3u4`:

```
ssh root@fe80::9683:c4ff:fea2:8ecb%enp3s0f3u4
```

**Windows**

Open Command Prompt (`Win+R`, type `cmd`, Enter). Run `ipconfig`. Under the adapter you are using, **Default Gateway** is the link-local, e.g. `fe80::9683:c4ff:fea2:8ecb%8` (the number after `%` is the interface index).

If `ssh` is not recognised: Settings → Apps → Optional Features → Add a feature → **OpenSSH Client**.

```
ssh root@fe80::9683:c4ff:fea2:8ecb%8
```

**After login**

Run `mesh11sd status` to confirm the node is healthy.

From a node that is already on the mesh, `mesh11sd connect` lists other reachable nodes by **label MAC** (and `mesh11sd connect [Node ID]` opens a remote shell).

Once at least one mesh peer is up, the power or status LED on most hardware shows a heartbeat. That is `mesh_backhaul_led='auto'`. If the device uses a non-standard LED, set `mesh_backhaul_led` later to a name from `/sys/class/leds` (format `color:function`), or `none` to disable it.

### 5.4 Node-by-node install

Flash stock OpenWrt **25.12** or later. WAN to the ISP, LAN to your PC, confirm Internet. Wireless can stay disabled; mesh11sd will enable it.

Package management is **apk** (not opkg):

```
apk update
apk del wpad-basic-mbedtls
apk add wpad-mbedtls
service wpad restart
apk add ip-full kmod-nft-bridge vxlan luci-ssl luci-app-commands mesh11sd
```

The daemon starts in manual mode (`auto_config=0`). Set the root password, then run the confidence test (section 5.1). Repeat on each node with the same `auto_mesh_id`.

### 5.5 After first contact

Stop the service before UCI edits (`service mesh11sd stop`), then:

```
uci set mesh11sd.setup.auto_mesh_id='MyMeshIDSeed'
uci set mesh11sd.setup.auto_mesh_key='MyMeshKeySeed'   # optional
uci set mesh11sd.setup.mesh_gate_encryption='3'
uci set mesh11sd.setup.mesh_gate_key='mysecretaccesscode'
uci commit mesh11sd
```

Country: set in wireless or `mesh11sd.setup.country` (overrides wireless). Unset → DFS-ETSI.

Portal IPv4: if `network.lan` is still `192.168.1.1`, auto-config hashes an RFC1918 address unless `portal_use_default_ipv4=1`.

Enable auto-config: `mesh11sd auto_config enable` or `uci set mesh11sd.setup.auto_config='1'`.

Power nodes in any order. **One** WAN to the ISP for a detect=1 portal.

## 6. Important behaviour

1. Mesh11sd uses **uci** for dynamic changes. In normal operation those are **not** written to `/etc/config`.
2. Editing `/etc/config/wireless` or `/etc/config/network` by hand while the daemon runs will usually break the mesh.
3. **LuCI does not show or support mesh11sd.** The daemon disables LuCI unless `auto_config=2` (which also `commit_all` and **locks** portal detect to the first-boot role). Reverse with `mesh11sd revert_all revert`.
4. Peers track the portal **channel**. The mesh interface is created in runtime UCI, not in `/etc/config/wireless`.
5. All nodes must share mesh id, key, and (via tracking) channel. Auto-config does this if `auto_mesh_id` / `auto_mesh_key` match.

## 7. Configuration - Setup Options

The UCI package `mesh11sd` has **two** stanzas:

1. `config mesh11sd 'setup'` — daemon and auto-config options (this section).
2. `config mesh11sd 'mesh_params'` — 802.11s/HWMP parameters ([§8](#8-configuration---mesh-parameter-options)).

The **source** file (comments included) is in [§12](#12-default-configuration-file). On a flashed node, the image **uci-defaults** script strips those comments, so `/etc/config/mesh11sd` is usually only the options you set in the first-boot script (for example `auto_config`, `debuglevel`, `portal_detect`) plus an empty `mesh_params` section.

`uci export mesh11sd` shows the **runtime** overlay. The daemon writes `mesh_params` (and many `setup` values) there with `uci batch` and does **not** commit them, so `cat /etc/config/mesh11sd` will not match `uci export` unless you `commit_all`.

Values in the tables below are script defaults when the option is unset. Set with `uci set mesh11sd.setup.<name>='…'` (commit only if you need it across a reboot **without** auto-config rewriting it).

### 7.1 Core

| Option | Default | Notes |
| --- | --- | --- |
| `enabled` | 1 | Daemon on/off. |
| `debuglevel` | **3** | 0 silent, 1 notice, 2 info, 3 debug. Also `mesh11sd debuglevel N`. |
| `checkinterval` | 10 | Seconds between reconfiguration passes. |
| `interface_timeout` | 10 | Wait for an interface to come up. |
| `auto_config` | 0 | 0 off, 1 on, 2 on + `commit_all` + LuCI (locks first role). |
| `portal_detect` | 1 | See [§4](#4-node-types). Ignored if auto_config is 0. |
| `portal_detect_threshold` | 10 | Watchdog: checkintervals without a portal before action. 0 = never. |
| `portal_use_default_ipv4` | 0 | Portal: 1 = keep `/etc/config/network` IPv4; 0 = hash from label MAC. |
| `portal_channel` | default | Portal 2.4 GHz: `auto`, `default`, or channel 1–13. Peers track it. |
| `channel_tracking_checkinterval` | 30 | Minimum seconds between peer channel scans. |

### 7.2 Mesh identity and radio

| Option | Default | Notes |
| --- | --- | --- |
| `auto_mesh_id` | `--__` | Seed hashed to mesh ID. **Same on every node.** |
| `auto_mesh_key` | hashed | Extra seed for SAE key. Same on every node if set. |
| `auto_mesh_band` | `2g40` | `2g`, `2g40`, `5g`, `6g`, `60g`. |
| `mesh_phy_index` | unset | Force `phyN` for the mesh vif. |
| `mesh_multi_interfaces` | disabled | Set `enabled` to allow more than one mesh vif. |
| `country` | DFS-ETSI if unset | Overrides wireless country. |
| `mesh_basename` | `11s` | First 4 alphanumerics → iface `m-11s-0`. |
| `auto_mesh_network` | `lan` | Firewall zone for the mesh. Not `wan` (CPE rewrites this internally). |
| `txpower` | driver | dBm; regulatory domain applies. CLI: `mesh11sd txpower +/-`. |

### 7.3 Mesh gates (user Wi-Fi)

| Option | Default | Notes |
| --- | --- | --- |
| `mesh_gate_enable` | 1 | 0 all APs off; 1 all on; 2 APs only on radios **without** a mesh vif. |
| `mesh_gate_base_ssid` | wireless SSID, or `MeshGate` | Max 22 chars if suffix on, else 30. |
| `mesh_gate_encryption` | **4 (OWE)** | 0 none/OWE-transition, 1 SAE, 2 sae-mixed, 3 psk2, 4 OWE. Falls back to 0 without full wpad. |
| `mesh_gate_key` | unset | Ignored for encryption 0 and 4. |
| `ssid_suffix_enable` | 1 | Last 4 hex digits of mesh MAC appended to SSID. |
| `mesh_leechmode_enable` | 0 | AP-only, no mesh forwarding. Mobility 0, not portal. CLI: `mesh11sd mesh_leechmode`. |

### 7.4 VXLAN vxtunnel (needs `ip-full` and `vxlan`)

| Option | Default | Notes |
| --- | --- | --- |
| `vtun_enable` | 1 (0 on CPE) | Point-to-multipoint vxlan. |
| `tun_id` | 69 | VNI / bridge `br-tun69`. 1–16777216. |
| `vtun_ip` | auto | Overlay IPv4 of the routed portal on the tunnel. |
| `vtun_mask` | 255.255.255.0 | |
| `vtun_base_ssid` | `VTunnel` | Guest SSID on the tunnel. |
| `vtun_gate_encryption` | 4 (OWE) | Same codes as mesh_gate_encryption. |
| `vtun_gate_key` | unset | Min 8 chars if used. |
| `vtun_path_cost` | **65525** | STP cost of the tunnel. 0 = do not set. |
| `vtun_isolate` | 0 | 1 = no forwarding between mesh/lan and vxtunnel (guest isolation). |
| `move_ethernet_to_vxlan_bridge` | 0 | Move LAN Ethernet onto `br-tun`. Not used in CPE. |

MBP/TPN: WAN is added to `br-tun$tun_id`. MRP portal may serve DHCPv4/RA on vtunlan; MBP/CPE/TPN/peers do not.

### 7.5 Path, watchdog, AP monitor, CPE

| Option | Default | Notes |
| --- | --- | --- |
| `mesh_path_cost` | **65525** | STP cost of the mesh. 0 = do not set. README 6.x “10” is obsolete. |
| `mesh_path_stabilisation` | 0 | Rarely needed if mobility level &gt; 0. |
| `reactive_path_stabilisation_threshold` | 10 | Checkintervals before reactive stabilisation. |
| `mesh_mac_forced_forwarding` | 1 | |
| `gateway_proxy_arp` | 1 | |
| `mesh_metric_threshold` | 50 | Portal-selection hysteresis (airtime metric). |
| `reboot_on_error` | 1 | Watchdog reboot if portal still missing. |
| `stop_on_error` | 0 | Idle instead of reboot (overrides reboot_on_error). |
| `watchdog_nonvolatile_log` | 0 | **Debug only** — can wear flash. |
| `apmond_enable` | 1 | AP/Ethernet stats to the portal. |
| `apmond_cgi_dir` | `/www/cgi-bin/` | |
| `apmon_verbose_debug_enable` | 0 | |
| `mesh_backhaul_led` | `auto` | `none` to disable; or `color:function` from `/sys/class/leds`. |
| `manage_opennds_startup` | 1 | If opennds is installed. |
| `log_mountpoint` | `/tmp` | Not on system flash for long-term logs. Subdir `mesh11sd`. |
| `max_log_entries` | 500 | `mesh11sd read_log`. |
| `use_default_beacon_interval` | 0 | 1 = keep driver 100 ms (some ath11k/ipq6018). |
| `mesh_dtim_period` | 2 | Overridden by mobility level unless the driver ignores it. |
| `mesh_node_mobility_level` | **2** | 0–6; see CLI. |
| `cpe_mode` | `nat66` | CPE only: `nat66`, `prefix_delegation`, `relay`. |
| `odhcpd_log_level` | 3 | 0–7. |
| `wpad_debuglevel` | 2 | 0–5; rewrites wpad init flags. |
| `connect_show_all` | 1 | `connect` with no args lists island meshes too. |

## 8. Configuration - Mesh Parameter Options

`config mesh11sd 'mesh_params'` holds 802.11s parameters. A minimum set is applied at startup if unset; the daemon re-applies them every `checkinterval`. Any parameter the wireless driver supports may be added.

**On disk vs runtime.** After image build, `/etc/config/mesh11sd` might typically have:

```
config mesh11sd 'setup'
	option auto_config '1'
	option debuglevel '3'
	option portal_detect '5'

config mesh11sd 'mesh_params'
```

`uci export mesh11sd` on a running node shows the live `mesh_params` (retry/confirm/holding timeouts, `mesh_rssi_threshold`, `mesh_hwmp_rootmode`, and so on). Those lines are **not** in the file unless you `mesh11sd commit_all commit`. Use `uci get mesh11sd.mesh_params.<name>` or `mesh11sd status` to see what is active.

To pin a value across reboot without `commit_all`, put it in the `mesh_params` stanza in `/etc/config/mesh11sd` (or in the Firmware Selector uci-defaults script).

Defaults applied when unset include `mesh_fwding=1`, `mesh_retry_timeout=255`, `mesh_confirm_timeout=255`, `mesh_holding_timeout=255`, `mesh_rssi_threshold=-63`, `mesh_gate_announcements=1`, `mesh_hwmp_rootmode=0` (portals then force **4**; peers typically **2**), `mesh_hwmp_root_interval=5000`, `mesh_hwmp_active_path_to_root_timeout=6000`, `mesh_max_peer_links=16`, `mesh_plink_timeout=500`. Mobility level also overwrites several HWMP timers at runtime.

Parameters and meaning:

 * **mesh_retry_timeout** — initial retry timeout (ms) for Mesh Peering Open
 * **mesh_confirm_timeout** — initial confirm timeout (ms) for Mesh Peering Open
 * **mesh_holding_timeout** — timeout (ms) used to close a mesh peering
 * **mesh_max_peer_links** — maximum peer links on this mesh interface
 * **mesh_max_retries** — maximum peer-link open retries
 * **mesh_ttl** — TTL set at a source mesh STA
 * **mesh_element_ttl** — TTL for path-selection elements
 * **mesh_auto_open_plinks** — auto-open peer links when compatible peers are seen (deprecated; usually hard-coded on)
 * **mesh_sync_offset_max_neighor** — (spelling as in the kernel) max neighbours to synchronise to
 * **mesh_hwmp_max_preq_retries** — PREQ action frames an originator may send toward one target
 * **mesh_path_refresh_time** — how often to refresh mesh paths (ms)
 * **mesh_min_discovery_timeout** — minimum time to wait before giving up path discovery (ms)
 * **mesh_hwmp_active_path_timeout** — how long a PREQ forwarding entry stays valid (ms)
 * **mesh_hwmp_preq_min_interval** — minimum interval between PREQ action frames (ms)
 * **mesh_hwmp_net_diameter_traversal_time** — time for an HWMP element to cross the mesh (ms)
 * **mesh_hwmp_rootmode** — this STA as HWMP root (portals: 4)
 * **mesh_hwmp_rann_interval** — interval between root announcements (ms)
 * **mesh_gate_announcements** — advertise access to a broader network beyond the MBSS
 * **mesh_fwding** — whether this STA forwards (0 in leech mode)
 * **mesh_rssi_threshold** — minimum average signal to establish a peer link (dBm, negative)
 * **mesh_hwmp_active_path_to_root_timeout** — validity of proactive PREQ info toward the root (ms)
 * **mesh_hwmp_root_interval** — interval between proactive PREQs (ms)
 * **mesh_hwmp_confirmation_interval** — minimum interval between root-path confirmation PREQs (ms)
 * **mesh_power_mode** — default mesh power-save mode for new peer links
 * **mesh_awake_window** — time (ms) the STA stays awake after its beacon
 * **mesh_plink_timeout** — if no TX from a peer for this many seconds, drop it (0 ≈ 30 minutes in the stack)
 * **mesh_connected_to_as** — advertise connectivity to an authentication server / upstream router
 * **mesh_connected_to_gate** — advertise connectivity to other infrastructure (AP, downstream router)
 * **mesh_nolearn** — avoid multi-hop discovery if the destination is a neighbour (not optimal for HWMP)

`mesh11sd status` lists the parameters the driver actually exposes.

## 9. Command line

The daemon runs as a service. CLI is for status, logs, and a few runtime knobs. Runtime UCI is **not** what ubus/`/etc/config` always shows; prefer `uci show` / `uci get` after `uci batch` changes, or `mesh11sd status`.

**Everyday**

| Command | Purpose |
| --- | --- |
| `mesh11sd status` | JSON setup, interfaces, peers, metrics |
| `mesh11sd connect` / `connect filter_list` / `connect [Node ID]` | List or SSH to a node (label MAC) |
| `mesh11sd copy` / `copy filter_list` / `copy [Node ID] [file]` | Copy file to `$tmpdir/` on a node |
| `mesh11sd stations` | One-hop mesh STAs |
| `mesh11sd show_ap_data all` | AP/Ethernet client stats (portal) |
| `mesh11sd get_ap_data` | Stats for this node |
| `mesh11sd read_log` / `read_log -f` | Internal log (`-f` truncates then follows) |
| `mesh11sd debuglevel [0-3]` | Get/set |
| `mesh11sd get_portal_type` | MBP, MRP, MPE, CPE, TPN |
| `mesh11sd get_portal_ula [get\|discover]` | Current or best portal ULA |
| `mesh11sd get_node_type_code` | Same codes as get_portal_type |
| `mesh11sd active_nodecount` | Peers in mpath |

**Runtime radio / mesh**

| Command | Purpose |
| --- | --- |
| `mesh11sd txpower +` / `-` | ±3 dBm immediately |
| `mesh11sd mesh_rssi_threshold +` / `-` `[force]` | ±3 dB; `force` drops peers briefly |
| `mesh11sd mesh_leechmode enable` / `disable` | |
| `mesh11sd mesh_node_mobility_level [0-6]` | 0 stationary; 1 RTS/AQL; 2 ~1.5 m/s; 3–5 higher speed/overhead; 6 PTT/voice |
| `mesh11sd country [CC]` | Show or set regulatory domain (reboot to apply) |
| `mesh11sd get_valid_channels` | Non-DFS channels for current country |
| `mesh11sd wifi_chipset_detect` | JSON phy capabilities |

**Service / persist**

| Command | Purpose |
| --- | --- |
| `mesh11sd enable` / `disable` | Daemon flag |
| `mesh11sd auto_config test` / `enable` / `disable` | Test reverts on reboot |
| `mesh11sd commit_changes commit` | Persist leechmode, txpower, rssi |
| `mesh11sd commit_all commit` | Write auto_config result to files |
| `mesh11sd revert_all revert` | Undo commit_all |
| `mesh11sd dhcp4_renew [ifname]` | Force DHCPv4 (default: mesh network device) |
| `mesh11sd set_ula_prefix get` / `set` / `revert` | Mesh ULA /48 |
| `mesh11sd force_ipv4_download` | Force apk (wget) downloads over IPv4 |
| `mesh11sd download_revert_to_default` | Undo that |
| `mesh11sd is_installed [pkg]` | Exit 0 if installed |

**Logging / apmond helpers**

`write_to_syslog`, `write_log`, `write_node_data`, `send_ap_data`, `str_to_hex`, `hex_to_str`, `is_hex`, `is_ipv4addr_valid`.

There is **no** `mesh11sd wireless channel` command (older README was wrong). Channel is `portal_channel` plus peer tracking.

`commit_changes` requires the argument `commit`. Mobility CLI supports **5 and 6**. `auto_config disable` exists. `connect` takes a **Node ID** (label MAC), not an arbitrary argument list.

## 10. HWMP peer status and MP_FLAGS

Hybrid Wireless Mesh Protocol (HWMP) is the mac-routing protocol of the backhaul. `mesh11sd status` shows **airtime metric**, **mpath metric**, and **MP_FLAGS** for each path.

Portals force `mesh_hwmp_rootmode` to **4** (proactive root). Peers typically use 2 (or 0 in leech mode).

**MP_FLAGS** is a bitmask (`NL80211_MPATH_FLAG_*` in the Linux kernel):

| Flag | Bit | Meaning |
| --- | --- | --- |
| ACTIVE | 0x1 | Used for forwarding |
| RESOLVING | 0x2 | Discovery in progress |
| SN_VALID | 0x4 | Sequence number valid |
| FIXED | 0x8 | Static path |
| ROOT | 0x10 | Root path (proactive HWMP) |

Typical values: `0x5` = ACTIVE+SN_VALID; `0x15` = +ROOT; `0x17` = +RESOLVING.

## 11. Mobility and airtime metric

Airtime link metric updates are driven mainly by **beacon interval** and HWMP PREQ timing. mesh11sd sets those from `mesh_node_mobility_level`:

| Level | Intent |
| --- | --- |
| 0 | Stationary; leechmode allowed |
| 1 | RTS, TX queue, AQL; slow movement |
| **2** (default) | About 1.5 m/s relative |
| 3–5 | Faster movement, more overhead |
| 6 | PTT / PA voice |

Some drivers (e.g. IPQ6018) break if beacon interval changes — use `use_default_beacon_interval=1`.

## 12. Default configuration file

This is the **source** UCI file shipped in the package (`linux_openwrt/mesh11sd/files/etc/config/mesh11sd`). Image **uci-defaults** strips comment lines, so the copy on the device is only the options you set plus an empty `mesh_params` section. Runtime values live in the UCI overlay (`uci export mesh11sd`); see [§7](#7-configuration---setup-options) and [§8](#8-configuration---mesh-parameter-options).

```
config mesh11sd 'setup'
	###########################################################################################
	# debuglevel (optional)
	# Sets the debuglevel
	# Default: 3 (debug)
	# Options are 0, silent, 1 notification, 2 info and 3 debug
	#
	#option debuglevel '1'

	###########################################################################################
	# enabled (optional)
	# Enables or disables the mesh11sd daemon
	# Default: 1 (enabled)
	#
	# Set to 0 to disable
	#
	#option enabled '1'

	###########################################################################################
	# checkinterval (optional)
	# Sets the dynamic configuration checkinterval in seconds
	# Default: 10 (seconds)
	#
	#option checkinterval '15'

	###########################################################################################
	# portal_detect (optional)
	# Ignored if auto_config is disabled.
	#
	# Default 1
	#
	# Possible values:
	#  0  - Forced Mesh Routed Portal (MRP). Force ipv4 nat and ipv6 routed Portal mode regardless of an upstream connection.
	#
	#  1  - Auto Detect Node. Detect if the meshnode is a portal, meaning it has an upstream wan link, or if it is a peer with upstream connection via the mesh backhaul.
	# 	If the upstream link is active, the router hosting the meshnode will serve both ipv4 and ipv6 dhcp into the mesh network.
	#		Such a node will be a Mesh Routed Portal (MRP)
	# 	If the upstream link is not connected, dhcp will be disabled and the meshnode will function as a layer 2 bridge on the mesh network.
	#		Such a node will be a Mesh Peer (MPE)
	#
	#  2  - Deprecated - no longer used - replaced by mode 5.
	#
	#  3  - Client Premises Equipment (CPE)
	#  	Also known as Customer Premises Equipment
	#  	This is a peer mode but treats the mesh backhaul as an upstream wan connection.
	#
	#  	A nat routed ipv4 lan is created with its own ipv4 subnet.
	#  	By default a nat66 routed ipv6 lan is created with its own ipv6 subnet.
	#
	#   	Depending on the upstream ISP, possible optional modes are prefix_delegation, relay and nat66, settable using the cpe_mode option (see below)
	#
	#  4  - Mesh Bridge Portal (MBP) node. Force Bridge vxlan trunk portal node
	#	This mode should be used if a bridged connection to the upstream ISP router is required (ie bridged/no-nat ipv4/ipv6 ).
	#	Functions in a similar way to 0, but forces BRIDGED rather than routed portal mode, ADDING the wan ethernet port to the vxtunnel bridge (default br-tun69)
	#
	#	The wan port will be an ethernet end point into the vxtunnel, supporting vlans if required.
	#
	#	The wan port and lan port(s) form independent layer 2 networks carried by the mesh backhaul to all peer meshnodes.
	#	the vxlan tunnel can be treated as a separate virtual ethernet tunnel to all mesh nodes.
	#
	#	ie the wan port on a mode 4 portal is the entry point of a virtual peer to multi-peer vxlan, supporting ethernet network connecting
	#	to the VTunnel access pints of all nodes as well as the wan ports of all mode 5 Trunk Peer Nodes.
	#	In normal use, the lan port could be patched to the upstream router. The wan port is the ethernet entry point of the vxlan tunnel.
	#	The vxlan tunnel an be used as if it was a port on a managed switch, supporting vlan tagging if required.
	#
	#  5  - Trunk Peer Node (TPN)
	#	This is a special case of a peer node where its wan port is an endpoint of the multi-peer vxlan.
	#	Compatible with portal nodes configured with portal_detect 0, 1 or 4.
	#
	#	The wan port will be an ethernet end point into the vxtunnel, supporting vlans if required.
	#
	#	The lan port(s) will be ethernet end points into the mesh backhaul and will NOT support vlans.
	#
	#
	# Has no effect if auto_config is disabled.
	#
	#option portal_detect '0'

	###########################################################################################
	# portal_channel (optional) Applies to 2.4 GHz band only
	#
	# If portal_detect is disabled (0), portal_channel can be set to:
	# 1. auto - a channel is auto selected
	# 2. default - the channel defined in /etc/config/wireless is used
	# 3. A valid 2.4 GHz channel (1 to 13, depending on the country setting)
	#
	# Default: default
	#
	# All mesh peer and mesh gate nodes will autonomously track the mesh portal channel
	# regardless of the configured auto_mesh_band
	# 
	#option portal_channel 'auto'
	# or
	#option portal_channel '4'

	###########################################################################################
	# portal_use_default_ipv4 (optional)
	# Effective only if node is a portal
	#
	# Default 0
	#
	# When set to 1, the default ipv4 address found in /etc/config/network is used
	# When set to 0 or not set, an ip subnet address is calulated based on the label mac address
	#
	#option portal_use_default_ipv4 '1'

	###########################################################################################
	# channel_tracking_checkinterval (optional)
	#
	# The minimum interval in seconds after which channel tracking begins on peer nodes
	# Values less than checkinterval are ignored
	#
	# Default: 30 seconds
	#
	# Example:
	#option channel_tracking_checkinterval '60'

	###########################################################################################
	# portal_detect_threshold (optional)
	#
	# This is the portal detect watchdog.
	#
	# The number of checkintervals before the portal detect watchdog begins actions to (re)establish a reconnection to a portal.
	#
	# Default 10 (watchdog is triggered after 10 iterations)
	# If set to 0, the watchdog is never triggered
	# Ignored if auto_config is disabled.
	#
	# Each time the peer node fails to detect the portal, a counter is incremented.
	# If the threshold is reached, the node will take various actions in an attempt to find the portal.
	# If the portal is still not detected, the watchdog will reboot the peer node.
	#
	# Example - Set threshold to 10 chekintervals:
	#option portal_detect_threshold '10'

	###########################################################################################
	# mesh_path_cost (optional)
	#
	# sets the Interface cost of the mesh network
	# Default: 65525
	# Can be set to any value from 0 to 65534
	# Setting to 0 disables path cost setting
	#
	# Example:
	#option mesh_path_cost '100'

	###########################################################################################
	# interface_timeout (optional)
	#
	# Sets the interface timeout interval in seconds
	# Default: 10 (seconds)
	#
	#option interface_timeout '10'

	###########################################################################################
	# auto_config (optional)
	#
	# Enables autonomous dynamic mesh configuration.
	# Auto configure mesh interfaces in the wireless configuration.
	# Default 0 (disabled). Set to 1 to enable.
	#
	# Possible values:
	# 0 Disabled
	# 1 Enabled
	# 2 Same as 1 but executes `commit_all` and enables LuCi
	#   Warning, this will lock the auto config to the initial autoconfigured mode.
	#   If you want a locked portal, ensure you have the upstream Internet connection active
	#   BEFORE first boot after reflash or install.
	#   For example if the meshnode initially configures as a portal,
	#   portal detect will be set to 0 permanently.
	#
	#   The effect of option 2 can be reversed by issuing the command `mesh11sd revert_all revert`
	#
	# When set to 0, the mesh11sd daemon will check for an existing mesh configuration.
	#
	# Warning: If an existing mesh configuration is found, it will be honoured even if it is incorrect.
	#   Manually configuring a mesh can soft brick the router if incorrectly done.
	#
	# Auto config can be tested using the command line function 'mesh11sd auto_config test'
	#   See the documentation for further information (Hint: try 'mesh11sd --help')
	#
	#option auto_config '1'

	###########################################################################################
	# auto_mesh_id (optional)
	#
	# Configure the mesh_id of the wireless interface(s) when auto_config is enabled
	# Default --__
	#
	# This string will be hashed to produce a secure mesh id
	# If set, it must also be set to the same value on every mesh node
	#
	#option auto_mesh_id 'MyMeshID'

	###########################################################################################
	# auto_mesh_band (optional)
	#
	# Configure the band to use for the mesh network
	# Valid values: 2g, 2g40, 5g, 6g, 60g
	# Default 2g40
	#
	# If set, it must also be set to the same value on every mesh node
	#
	# All mesh peer and mesh gate nodes will autonomously track the mesh portal channel
	# regardless of the configured auto_mesh_band
	#
	#option auto_mesh_band '5g'

	###########################################################################################
	# mesh_phy_index (optional)
	#
	# Force use of a particular radio for the mesh interface
	# Must be an integer value corresponding to the physical radio hardware (eg. phy0, phy1 etc.).
	# Default - Not Set
	#
	# Useful for devices with more than one phy on a particular band
	# allowing use of a particular radio to be forced
	#
	# If not set, the first phy in the configured auto_mesh_band that the daemon encounters
	# will be used for the mesh interface.
	#
	# Example - Use the second 5GHz radio (phy2) of a three radio device:
	#option mesh_phy_index '2'

	###########################################################################################
	# mesh_multi_interfaces (optional)
	#
	# When set to 'enabled', more than one mesh interface may be brought up
	# (for example on more than one radio).
	# Default: disabled
	#
	#option mesh_multi_interfaces 'enabled'

	###########################################################################################
	# country (optional)
	#
	# Set a valid country code for all radios
	# Defaults to DFS-ETSI if not explicitly set in wireless config
	#
	# If set here, will override any setting in wireless config
	#
	# Example set to country code US:
	#option country 'US'

	###########################################################################################
	# auto_mesh_key (optional)
	#
	# Defaults to a sha256 key to be automatically used on all members of this mesh when auto_config is enabled
	# Generates a secure sha256 key from the string value set in this option.
	#
	# If set here, it must also be set to the same value on every other node in this mesh
	#
	#option auto_mesh_key 'MySecretKey'

	###########################################################################################
	# auto_mesh_network - (optional) - specifies the firewall zone used for the mesh.
	#	Typical values "lan", "guest" etc.
	#
	#	This can be set differently on each meshnode as required.
	#
	#	Firewall zone "wan" is not valid.
	#
	#	Default lan

	###########################################################################################
	# mesh_basename (optional)
	# The first 4 characters after non alphanumerics are removed are used as the mesh_basename
	# The mesh_basename is used to construct a unique mesh interface name of the form m-xxxx-n
	# Default: 11s
	# Results in ifname=m-11s-0 for the first mesh interface
	# Example: link
	# Results in ifname=m-link-0
	#
	#option mesh_basename 'link'

	###########################################################################################
	# mesh_gate_base_ssid
	#
	# Sets the mesh gate base ssid string
	#	If ssid_suffix_enable is set to 0, must be a maximum of 30 characters in length
	#	If ssid_suffix_enable is set to 1, must be a maximum of 22 characters in length
	#	Excess characters will be truncated
	#
	# Default:
	#	1 - uses the ssid string set in the wireless config if it is NOT set to OpenWrt
	#	2 - uses the ssid string MeshGate if the SSID string in the wireless config is OpenWrt
	#
	#
	# When set, overrides the ssid string set in the wireless config
	#
	# Example:
	#option mesh_gate_base_ssid 'OfficeNetwork'

	###########################################################################################
	# mesh_gate_encryption (optional)
	# Determines whether this node's gate (Access Point) will be a encrypted
	#
	# Default: 4 (Opportunistic Wireless Encryption - owe)
	# Set to 0 (none/owe-transition), 1 (sae, aka wpa3), 2 (sae-mixed, aka wpa2/wpa3), 3 (psk2, aka wpa2)
	# or 4 (Opportunistic Wireless Encryption - owe)
	#
	# Example - enable psk2 encryption
	#option mesh_gate_encryption '3'

	###########################################################################################
	# mesh_gate_key (optional)
	# Determines the encryption key for this node's gate.
	#
	# Default: not set (encryption disabled)
	# Set to a secret string value to use for encrypting the node's gate
	# Ignored if mesh_gate_encryption is set to 0 or 4
	#
	# Example - set a key string
	#option mesh_gate_key 'mysecretencryptionkey'

	###########################################################################################
	# ssid_suffix_enable (optional)
	# Add a 4 digit suffix to the ssid
	# The 4 digits are the last 4 digits of the mac address of the mesh interface
	# Default 1 (enabled)
	#
	#option ssid_suffix_enable '0'

	###########################################################################################
	# vtun_enable (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Enables point to multi-point vxlan tunneling from portal to all compatible nodes
	#
	# Default: 1 (enabled) unless portal_detect is set to 3 (cpe mode), in which case the default is 0 (disabled)
	#
	# To disable, set to zero:
	#option vtun_enable '0'

	###########################################################################################
	# tun_id (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel id, a decimal number between 1 and 16777216 (24 bits)
	#
	# Default: 69
	#
	#option tun_id '42069'

	###########################################################################################
	# vtun_ip (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel ipv4 gateway address to be used in the vxtunnel.
	# Becomes active if the node becomes a portal (portal_detect 0 or 1).
	#
	# Default: auto generated subnet
	#
	#option vtun_ip '10.69.1.1'

	###########################################################################################
	# vtun_mask (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel ipv4 address mask to be used in the vxtunnel.
	# Becomes active if the node becomes a portal (portal_detect 0 or 1).
	#
	# Default: 255.255.255.0
	#
	#option vtun_mask '255.255.0.0'

	###########################################################################################
	# vtun_gate_encryption (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel gate encryption to be used on node gates (access points) connected to the vxtunnel.
	#
	# Default: 4 (Opportunistic Wireless Encryption - owe)
	# Set to 0 (none/owe-transition), 1 (sae, aka wpa3), 2 (sae-mixed, aka wpa2/wpa3), 3 (psk2, aka wpa2)
	# or 4 (Opportunistic Wireless Encryption - owe)
	#
	#option vtun_gate_encryption '3'

	###########################################################################################
	# vtun_gate_key (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel gate encryption key to be used on node gates (access points) connected to the vxtunnel.
	#	Must be a minimum of 8 characters in length
	#
	# Default: not set
	#
	#option vtun_gate_key 'mysecretkey'

	###########################################################################################
	# vtun_base_ssid (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel base ssid string
	#	If ssid_suffix_enable is set to 0, must be a maximum of 30 characters in length
	#	If ssid_suffix_enable is set to 1, must be a maximum of 22 characters in length
	#	Excess characters will be truncated
	#
	# Default: VTunnel
	#
	#option vtun_base_ssid 'GuestNetwork'

	###########################################################################################
	# vtun_path_cost (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# sets the Interface cost of the vxtunnel network
	# Default: 65525
	# Can be set to any value from 0 to 65534
	# Setting to 0 disables path cost setting
	#
	# Example:
	#option vtun_path_cost '100'

	###########################################################################################
	# vtun_isolate (optional)
	#
	# When set to 1, forwarding between the mesh/lan zone and the vxtunnel zone is blocked.
	# Guest (VTunnel) clients then cannot reach the mesh LAN, only the portal uplink.
	#
	# Default: 0 (disabled)
	#
	#option vtun_isolate '1'

	###########################################################################################
	# mesh_gate_enable (optional)
	# Determines whether this node will be a gate
	#
	# Default: 1 (enable all mesh gate access points)
	#
	# Possible values:
	# 0 Disable all mesh gate access points.
	# 1 Enable all mesh gate access points.
	# 2 Enable ONLY access points on radios NOT shared with a mesh interface.
	#
	# Example:
	#option mesh_gate_enable '0'

	###########################################################################################
	# mesh_leechmode_enable (optional)
	# Determines whether this node will be a gate only leech node
	#
	# Valid only with mesh_node_mobility_level = 0 and not on a portal node.
	#
	# A gate only leech node acts as an access point with a mesh backhaul connection, but does not contribute to the mesh
	#
	# This is useful when a node is well within the coverage of 2 or more peer nodes,
	# as otherwise it could create unstable multi hop paths within the backhaul.
	#
	# Can also be set dynamically using the command line option 'mesh11sd mesh_leechmode [enable/disable]'
	#	See the documentation for further information (Hint: try 'mesh11sd --help')
	#
	# Default: 0 (disabled)
	# Set to 1 to enable (turns off the node's mesh forwarding and active HWMP mac-routing)
	#
	# Example - enable leach mode
	#option mesh_leechmode_enable '1'

	###########################################################################################
	# txpower (optional)
	# Set the mesh radio transmit power in dBm.

	# Default - use driver default or value set in wireless config
	# Values outside the limits defined by the regulatory domain will be ignored
	#
	# Example - Set tx power to 15 dBm:
	#option txpower '15'

	###########################################################################################
	# watchdog_nonvolatile_log (optional - FOR DEBUGGING PURPOSES ONLY)
	#
	# This enables logging of the portal detect watchdog actions in non-volatile storage.
	# The log file /mesh11sd_log/mesh11sd.log is created
	#
	# ##########WARNING##########
	# THIS OPTION IS FOR PORTAL DETECT WATCHDOG DEBUGGING PURPOSES ONLY
	# IF LEFT ENABLED FOR A LENGTH OF TIME IT MAY CAUSE NONE REPAIRABLE FLASH MEMORY WEAR AND USE UP FREE STORAGE SPACE
	# DISABLE IMMEDIATELY AFTER DEBUGGING OPERATIONS ARE COMPLETE
	# ##########WARNING##########

	###########################################################################################
	# mesh_path_stabilisation (optional)
	#
	# This enables mesh path stabilisation, preventing multi hop path changes due to multipath signal strength jitter
	# Usually not required when mesh_node_mobility_level is set to greater than 0
	#
	# Default: 0 (disabled)
	#
	# To enable, set to 1:
	#option mesh_path_stabilisation '1'

	###########################################################################################
	# reactive_path_stabilisation_threshold (optional)
	#
	# If an unstable path to an immediate neighbour node is detected, a counter is incremented each checkinterval while the unstable condition continues.
	# 	Mesh path stabilisation is activated once the counter exceeds the threshold.
	# Usually not required when mesh_node_mobility_level is set to greater than 0
	#
	# Default: 10 checkinterval periods.
	#
	#option reactive_path_stabilisation_threshold '5'

	###########################################################################################
	# mesh_mac_forced_forwarding (optional)
	#
	# This enables mac forced forwarding on the mesh interface
	#
	# Default: 1 (enabled)
	#
	# To disable, set to zero:
	#option mesh_mac_forced_forwarding '0'

	###########################################################################################
	# gateway_proxy_arp (optional)
	#
	# This enables proxy arp on the gateway bridge interface
	#
	# Default: 1 (enabled)
	#
	# To disable, set to zero:
	#option gateway_proxy_arp '0'

	###########################################################################################
	# stop_on_error (optional)
	#
	# If the watchdog detects a failure of ipv4 communication with a portal, the daemon will go into idle mode
	# This is useful if the meshnode does not have a reset button and a critical error occurs, blocking access
	#
	# Default: 0 (disabled)
	#
	# To enable, set to 1. This setting will overide the reboot_on_error setting
	#option stop_on_error '0'

	###########################################################################################
	# reboot_on_error (optional)
	#
	# If the watchdog detects a failure of ipv4 communication with a portal, the daemon will reboot the node
	#
	# Default: 1 (enabled)
	#
	# To disable, set to zero:
	#option reboot_on_error '0'

	###########################################################################################
	# apmond_enable (optional)
	#
	# Enables the access point monitoring daemon
	# 	Assumes the uhttpd and px5g-mbedtls (or luci-ssl) packages are installed.
	#	But other portal based https web servers can be used.
	# Default: 1 (enabled)
	#
	# Data is collected from access point interfaces on this node and sent to the portal node
	#
	# Example:
	#option apmond_enable '0'

	###########################################################################################
	# apmond_cgi_dir (optional)
	#
	# Sets the apmond cgi directory
	#	Takes effect when this node becomes a portal (portal detect 0, 1 and 4)
	# Default: /www/cgi-bin
	#
	# Example:
	#option apmond_cgi_dir '/webroot/cgi'

	###########################################################################################
	# mesh_backhaul_led (optional)
	#
	# Enables the mesh backhaul heartbeat led indicator
	#	The led indicator will be on when the mesh interface is up, changing to the Linux heartbeat signal when peer nodes are connected
	# Default: auto
	#	By default, the power or system led will be used if present.
	#
	#	Other leds can be found listed in /sys/class/leds with the format "color:function"
	# Disable this option by setting its value to "none"
	#
	# Example, enable the "blue:run" led:
	#option mesh_backhaul_led 'blue:run'
	#
	# Example, disable the mesh backhaul led:
	#option mesh_backhaul_led 'none'

	###########################################################################################
	# manage_opennds_startup (optional)
	#
	# Enables management of opennds startup
	#	If opennds is installed, mesh11sd will manage its startup.
	# Default: 1 Enabled
	#
	# Synchronizes nft rulesets of opennds and mesh11sd
	# Disabling may cause crash loops because the opennds gatewayinterface may not be up as mesh11sd starts.
	#
	# Example:
	#option manage_opennds_startup '0'

	###########################################################################################
	# log_mountpoint (optional)
	#
	# Specifies the mountpoint of storage to be used for the mesh11sd logging system
	# Default: /tmp
	# A subdirectory "mesh11sd" will be created in the mountpoint where all logs and temporary files will be stored
	# Ensure this mountpoint is NOT in system flash memory as it will lead to premature flash failure
	# Typically a removable (and replaceable) usb drive is ideal.
	#
	# Example:
	#option log_mountpoint '/logdrive'

	###########################################################################################
	# max_log_entries (optional)
	#
	# Specifies the number of rolling log entries to be kept by the logging system (displayed by the read_log cli command)
	# Default: 500
	# Log entries are stored in the mesh11sd directory on the log_mountpoint
	#
	# Example:
	#option max_log_entries '1000'

	###########################################################################################
	# use_default_beacon_interval (optional)
	#
	# When set, forces the use of the default beacon interval on the mesh phy
	# For most drivers, the beacon interval defaults to 100ms
	# mesh11sd dynamically sets the beacon interval according to the mesh_node_mobility_level setting
	# Some wireless drivers fail if the beacon interval is changed (eg Qualcomm Atheros IPQ6018)
	# Default: 0 (disabled)
	# Set to 1 to force
	#
	# Example:
	#option use_default_beacon_interval '1'

	###########################################################################################
	# mesh_dtim_period (optional)
	#
	# Sets the mesh DTIM period
	# DTIM - Discovery Timeout, Total timeout (in ms) for path discovery attempts.
	# A discovery beacon is sent every mesh_dtim_period beacons.
	# A larger value will slow the discovery process but reduce the overhead.
	# mesh11sd dynamically sets the mesh_dtim_period according to the mesh_node_mobility_level setting
	# Some wireless drivers ignore this option and continue to use the default (eg Qualcomm Atheros IPQ6018)
	# Default: 2
	#
	# Example:
	#option mesh_dtim_period '3'

	###########################################################################################
	# mesh_node_mobility_level (optional)
	#
	# Sets the mesh node mobility level
	# Supported levels are 0, 1, 2, 3, 4, 5 and 6
	# Level 0 - not recommended for normal use - node must be stationary and carefully positioned
	# Level 1 - Enables mesh_hwmp_rts for on air collision avoidance, enables transmit queue and aql_threshold to minimise latency, enables rapid path convergence
	# Level 2 - supports inter node relative velocities up to 1.5 metres per second
	# Levels 3 to 5 - support progressively higher relative inter node velocities at the expense of a larger backhaul overhead
	# Level 6 - PA / voice optimised (push-to-talk announcement use-case)
	#
	# Default: 2
	#
	# Example:
	#option mesh_node_mobility_level '2'

	###########################################################################################
	# cpe_mode (optional)
	#
	# Sets the cpe ipv6 mode
	# Applicable only when portal_detect is set to 3
	#
	# Possible modes are prefix_delegation, relay and nat66
	#
	# Prefix Delegation works with all client devices, including Android,
	#	but the ISP needs to provide a prefix large enough to delegate a /64 subnet to every cpe mesh node.
	#	Exhausting available delegations is a danger.
	#
	# Relay does not require any prefix delegation, but some versions of Android devices will detect the relay and turn off the device's interface within ~60 seconds - because - Google.
	#
	# NAT66 will work with all types of client devices, including Android, so is used as the default.
	# Default: nat66
	#
	#
	# Example:
	#option cpe_mode 'prefix_delegation'

	###########################################################################################
	# apmon_verbose_debug_enable (optional)
	#
	# Enables apmon verbose debug logging.
	# Logs can be read using the mesh11sd read_log command

	# Default 0 (disabled)
	#
	# Example: Enable verbose logging
	#option apmon_verbose_debug_enable '1'

	###########################################################################################
	# odhcpd_log_level (optional)
	#
	# Sets the odhcpd log level
	# Used for monitoring ipv6 dhcp/ra
	#
	# Can be set from 0 to 7
	# 0 - Emergency
	# 1 - Alert
	# 2 - Critical
	# 3 - Error
	# 4 - Warning
	# 5 - Notice
	# 6 - Info
	# 7 - Debug
	#
	# Logs can be read using the logread command

	# Default 3 (Error)
	#
	# Example: Set log level to debug
	#option odhcpd_log_level '7'

	###########################################################################################
	# wpad_debuglevel (optional)
	#
	# Sets wpa_supplicant verbosity via the wpad init script.
	# 0-2 QUIET/ERROR/WARNING, 3 INFO, 4 DEBUG, 5 EXCESSIVE
	# Default: 2
	#
	#option wpad_debuglevel '3'

	###########################################################################################
	# move_ethernet_to_vxlan_bridge (optional)
	#
	# When enabled, physical lan ethernet ports are moved onto the vxlan trunk bridge.
	# Not used in CPE mode (portal_detect 3).
	# Default: 0 (disabled)
	#
	#option move_ethernet_to_vxlan_bridge '1'

	###########################################################################################
	# connect_show_all (optional)
	#
	# When 1, `mesh11sd connect` with no Node ID lists reachable nodes including island meshes.
	# When 0, only nodes in this contiguous mesh are listed (same as `connect filter_list`).
	# Default: 1
	#
	#option connect_show_all '0'

	###########################################################################################
	# mesh_metric_threshold (optional)
	#
	# Hysteresis for portal selection (get_portal_ula).
	# A candidate must beat the current portal by at least this airtime metric delta.
	# Default: 50
	#
	#option mesh_metric_threshold '50'

config mesh11sd 'mesh_params'
	# A minimum set of parameters is automatically set for initial startup and do not have to be configured here
	#
	# Any mesh parameter supported by the wireless driver can be specified here
	# and will be dynamically set and continuously checked every "checkinterval"
	# 
	# Examples:
	#option mesh_rssi_threshold '-70'
	#option mesh_max_peer_links '20'

	#
	# The command: "mesh11sd status" gives a full list of supported parameters.
```

## 13. Acronyms

**STA** station · **PREQ** path request · **HWMP** Hybrid Wireless Mesh Protocol · **RANN** root announcement · **RSSI** signal · **ULA** unique local IPv6 · **3dRV** three-dimensional relative velocity · **MBP** Mesh Bridge Portal · **MRP** Mesh Routed Portal · **MPE** Mesh Peer · **CPE** Client Premises Equipment · **TPN** Trunk Peer Node · **OWE** Opportunistic Wireless Encryption

![mesh11sd](https://github.com/openNDS/mesh11sd/blob/master/docs/images/avatarsmall.png)

Further help: `mesh11sd --help` and https://github.com/openNDS/mesh11sd#readme
