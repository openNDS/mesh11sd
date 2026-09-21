# Getting started with mesh11sd

This guide is for a first mesh. It explains what mesh11sd does, which node type to use, and the smallest set of options you must set. Full option and CLI lists are in [README.md](README.md).

This documentation applies to mesh11sd **7.2.x** on **OpenWrt 25.12** and later (apk; not backported to 24.10 or earlier).

## What it does

Mesh11sd is an OpenWrt daemon that builds and runs an **IEEE 802.11s** wireless mesh backhaul. User devices (phones, laptops) do **not** join 802.11s. They join a normal access point (a **mesh gate**) on a mesh node. The mesh carries traffic between gates and the Internet.

On each boot it:

- Creates a hashed, encrypted 802.11s mesh (same `auto_mesh_id` / `auto_mesh_key` on every node)
- Chooses a **node type** from `portal_detect` and whether WAN has an upstream
- Brings up mesh-gate SSIDs (OWE by default)
- Optionally builds a VXLAN **vxtunnel** (guest / trunk) over the backhaul
- Watches the mesh and reconfigures when links appear or disappear

Configuration is held in **runtime UCI**, not written to `/etc/config` unless you `commit_all` or set `auto_config` to `2`. Do not edit `/etc/config/wireless` by hand for the mesh.

**802.11s is not 802.11r.** Mesh is the backhaul. Client roaming is a different standard.

## What you need

- Two or more OpenWrt devices with 802.11s-capable radios
- Preferably WAN **and** LAN Ethernet (for a portal, CPE, or trunk)
- Packages for full function: `wpad-mbedtls` (not `wpad-basic-mbedtls`), `luci-ssl`, `luci-app-commands`, `ip-full`, `kmod-nft-bridge`, `vxlan`

If the image uses `ath10k-ct`, switch to the non-ct ath10k packages or mesh11sd will refuse to run.

## Node types (`portal_detect`)

Set the same `auto_mesh_id` (and optional `auto_mesh_key`) on every node. Choose `portal_detect` per role.

| Value | Code | Role | Typical Ethernet |
| --- | --- | --- | --- |
| **1** (default) | MRP or MPE | **Auto detect.** WAN up → Mesh Routed Portal (NAT IPv4, routed IPv6, DHCP/RA into the mesh). WAN down → Mesh Peer (layer-2 bridge, DHCP client on the mesh). | Portal: WAN to ISP/LAN. Peer: nothing required; LAN is the mesh. |
| **0** | MRP | Always a routed portal, even with no WAN. | WAN to ISP if you have one. |
| **3** | CPE | Customer / client premises. Mesh is the **WAN**. LAN is a private NAT network (IPv4 + NAT66 by default). | Laptop/LAN clients on **LAN**. Mesh on WAN/radio. |
| **4** | MBP | Mesh Bridge Portal. No IPv4 NAT; LAN is bridged to the existing LAN/ISP. WAN is the **vxlan trunk** (not the ISP). | **LAN** patched to the ISP/existing LAN. **WAN** is the trunk. |
| **5** | TPN | Trunk Peer. LAN is the mesh (no VLANs). WAN is a vxlan trunk endpoint (VLANs OK). | Compatible with `portal_detect` 0, 1, or 4. |
| **2** | — | Unused (replaced by 5). | — |

Start every node on **`portal_detect=1`** unless you know you need CPE, MBP, or TPN.

**Simplest home mesh:** one node with WAN plugged into the ISP router (`portal_detect=1` becomes MRP). Other nodes unplugged from WAN (`portal_detect=1` becomes MPE). Same `auto_mesh_id` on all.

## Confidence test (recommended)

A bad mesh config can lock you out. Mesh11sd can run auto-config **once** so a power cycle restores the previous state.

1. Flash OpenWrt (or a custom image) with mesh11sd installed.
2. Leave `auto_config` at **0**.
3. SSH in (the first session may still use `192.168.1.1`) and run:

```
mesh11sd auto_config test
exit
```

Do **not** wait on that SSH session. As the daemon starts it changes the IPv4 subnet, so the session will hang if you leave it open. `debuglevel` already defaults to 3.

4. Reconnect as in [Accessing a node](#accessing-a-node) below, then run `mesh11sd status`. If the node looks healthy, set `auto_config` to `1` (or `mesh11sd auto_config enable`) so auto-config runs on every boot. If you lose access, **power cycle**.

## First mesh (rapid image)

Preferred path: [OpenWrt Firmware Selector](https://firmware-selector.openwrt.org/).

1. Choose model and **OpenWrt 25.12** (or later).
2. Customize packages:
   - Prefix `wpad-basic-mbedtls` and `luci` with `-` (remove).
   - Add: `wpad-mbedtls luci-ssl luci-app-commands ip-full kmod-nft-bridge vxlan mesh11sd`
3. First-boot script (uci-defaults), **confidence test** (`auto_config=0`):

```
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

Use the **same** `auto_mesh_id` (and `auto_mesh_key` if you set one) on every hardware image.

4. Request the build, flash every node of that hardware type.
5. Plug **one** node's **WAN** into the ISP LAN. Power it on. That node becomes the portal.
6. SSH using the node's **IPv6 link-local** (see [Accessing a node](#accessing-a-node)). IPv4 on the portal is hashed from the label MAC and is not `192.168.1.1`.
7. Run `mesh11sd status`. Then place the other nodes and power them on.

When you are confident, set `auto_config='1'` in the image or with `mesh11sd auto_config enable`.

`auto_config='2'` is the same as 1 **and** runs `commit_all` and enables LuCI. It **locks** the first-boot portal/peer role. Reverse with `mesh11sd revert_all revert`.

## CPE image (community / WISP)

Use `portal_detect=3` only on the customer node. The portal stays `portal_detect` `0` or `1`.

```
uci set mesh11sd.setup.auto_config='1'
uci set mesh11sd.setup.portal_detect='3'
uci set mesh11sd.setup.auto_mesh_id='MyMeshID'
uci set mesh11sd.setup.mesh_gate_base_ssid='MyNetwork'
uci set mesh11sd.setup.mesh_gate_encryption='1'
uci set mesh11sd.setup.mesh_gate_key='MyWifiCode'
uci commit mesh11sd
```

CPE LAN is a private subnet. IPv6 mode is `cpe_mode` (`nat66` default; also `prefix_delegation` or `relay`).

## Options you should set on every node

| Option | Why |
| --- | --- |
| `auto_mesh_id` | Seed for the hashed mesh ID. **Same on every node.** |
| `auto_mesh_key` | Optional extra seed for the mesh SAE key. Same on every node. |
| `country` | Legal radio domain (default DFS-ETSI if unset). |
| `mesh_gate_base_ssid` | User Wi-Fi name (max 22 chars if suffix enabled). |
| `mesh_gate_encryption` / `mesh_gate_key` | User Wi-Fi security (default OWE = 4; 1=WPA3, 3=WPA2). |
| `auto_mesh_band` | Mesh radio band (`2g40` default; also `2g`, `5g`, `6g`, `60g`). Peers track the portal channel. |

Everything else can wait. Defaults: `portal_detect=1`, `mesh_node_mobility_level=2`, `vtun_enable=1` (guest VTunnel SSID), `apmond_enable=1`.

## Everyday commands

```
mesh11sd status                 # JSON: setup, mesh ifaces, peers
mesh11sd connect                # list nodes (label MACs); connect [Node ID]
mesh11sd copy [Node ID] [file]  # copy a file to /tmp/mesh11sd on a peer
mesh11sd read_log -f            # follow the mesh11sd log
mesh11sd show_ap_data all       # AP client stats (on a portal)
mesh11sd get_portal_type        # MRP, MBP, MPE, CPE, TPN
```

`mesh11sd connect` uses **factory/label MACs** (the mesh vif uses the label MAC so `iw mpath` matches the sticker).

## Accessing a node

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

## If something goes wrong

- Power cycle a node that was only in `auto_config test`.
- Portal IPv4 is not `192.168.1.1`; use link-local IPv6 or `mesh11sd connect` from another node.
- LuCI is disabled while auto-config is running unless `auto_config=2`.
- After changing `portal_detect`, restart the daemon (`service mesh11sd restart`) and wait a few `checkinterval`s (default 10s).
- Watchdog: if a peer cannot see a portal, it may DHCP-renew, restart the network, then reboot (`reboot_on_error=1`). Set `portal_detect_threshold=0` to disable the watchdog.

Next: [README.md](README.md) for the full option list, CLI, HWMP flags, and firmware-selector detail.
