# Implementation status for this working tree

Packaging identifier: `0.0.0_pre8-r0`. Unpublished. No tag and no feed entry.

## Present in the C sources

- Read-only planner and factory-MAC identity chain
- Explicit OpenWrt peer, auto-IPv4 boot role, and IPv4-only MRP entry points
- Owned bridge with STP off, owned procd runtime data, and base
  `libuci` / `libubus` / `libubox` / `libblobmsg-json` only
- Queryable `mesh11sd.v8` `status` object and a thin procd init
- Install-time refusal of enable and start

## Inactive or unfinished

| Item | State in this tree |
|---|---|
| Loop protection | Inactive. No STP and no nftables loop rule |
| Bridged-client multi-portal ARP | Unimplemented. Status reports it false |
| Dynamic peer promotion and reentry | Explicitly deferred. A running peer does not promote when WAN appears. A lost MRP qualification withdraws and exits |
| IPv6 RA, GUA, MBP | Unimplemented. Those profiles fail closed |
| ACS and channel migration | Unimplemented |
| Hardware | First intended pair is two GL-MT3600BE boards. Flashed firmware compatibility is unverified |
| Internet reachability | Own-WAN configured-DNS qualification only |

## Checks

| Check | Where it has run |
|---|---|
| 864 host checks, 3 dnsmasq 2.93 parser cases, private ubusd, 16 init fixtures | Parent box, on the previous layout archive, after restoring `build/references/dnsmasq-2.93.tar.xz` at SHA256 `0c00d4e5c97c8306e5fb932b348b34269c9c29a0e7df0e8e82958b407092bc19` |
| Sanitizer, analyzer, and the old three-target naming loop | The r2 retest passed sanitizer and analyzer. Naming no longer requires ath79 or mt76x8 outputs |
| Three 25.12.6 SDK APKs | Parent box, scratch `TARGET_CC` recipe, before this layout. Checksums are in `openwrt/mesh11sd/VALIDATION.md` |
| This packaging correction | Not built here. No SDK on this machine |

The scratch narrative, including the counts above, is `docs/SCRATCH-MILESTONE.md`.
