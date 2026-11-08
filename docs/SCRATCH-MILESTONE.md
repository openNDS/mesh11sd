# Scratch milestone record (pre-layout)

This file is the milestone record from the standalone scratch tree, kept so the
limitations and the checks that were run there stay visible. It is not the
current user guide. Paths below that say `beta/`, `packaging/`, or `../slice1/`
are the old scratch paths.

Current layout: shared C in `src/lib`, daemon entry points in `src/mesh11sd`,
tests in `tests`, OpenWrt recipe in `openwrt/mesh11sd`, host checks from the
root `CMakeLists.txt`. `sha256.c` moved from `slice1/` into `src/lib`, and the
three `#include "../slice1/sha256.h"` lines now include `"sha256.h"`. No other
C source line was edited for the layout move. The original slice1 LLDP/ferry
suite was not part of this handoff and was not rerun.

Checks named in the record below were run on the parent build box before this
layout move. This checkout has not re-run that native, sanitizer, analyzer, or
SDK suite. See `README.md`.

# mesh11sd v8 beta-r1 — local experimental SDK APK checkpoint

Standalone scratch milestone: actual disabled-install experimental APKs now built
for three matching OpenWrt25.12.6 SDK targets; **not a hardware-verified mesh or
complete v7 replacement**. See [packaging validation](packaging/VALIDATION.md).
There is now an actual opt-in OpenWrt foreground peer/IPv4-MRP engine, not merely
a mock apply contract. It has not been enabled on any router or the box.
All additions are in this `beta/` directory. The existing 27-test LLDP/ferry
prototype in `../slice1/` is preserved, not folded into auto-configuration.
No repository, live router, laptop, host network interface or mesh UCI state was changed.
Authorized host-only SDK prerequisites were installed from authenticated Debian
packages; daemon activation was blocked. No mesh service/APK was installed.

## Implemented

- `factory.c`, `factory.h`: independent six-stage factory identity selection:
  device tree (sysfs, proc, pre-jail copy, `mac-address` before
  `local-mac-address`), board.json system label, first UCI system label, WAN
  permanent MAC, eth1/eth0/lan0/lan1 universal permanent MAC, then lowest-ifindex
  Ethernet universal permanent MAC. Uses supplied, parsed evidence; no libapmon.
  Rewritten current addresses are never substituted for permanent identity;
  bridge fallback, zero/multicast addresses and malformed inventory are refused.
  `mesh11sd_plan_from_evidence()` connects this to the actual planner.
- `identity.c`: bit-for-bit existing v7/slice1 SHA-256 ULA algorithm. Shared /48
  comes from the common group; LAN and VXLAN hints come from factory MAC with
  its original trailing newline. Portal address is per-node subnet `::1`.
  Management IID comes from the **actual bridge MAC**, not station/factory MAC;
  preserves the legacy unflipped U/L bit. No SAE/crypto implementation.
- `plan.c`, `mesh_config.h`: validated, deterministic, transactional pure-mesh
  configuration planning. Explicit radio/channel/security and address/name
  overrides are preserved; unique eligible radio may be selected automatically.
  The pure core refuses ambiguous inventories; the native adapter now follows
  recovered legacy configured-radio order within the selected band. Refuses unsupported/disabled/NO_IR/DFS channels and unverified
  interface combinations instead of asserting that they work. Peer DHCP client
  and routed-portal static IPv4/DHCP policy are represented. Routed portals use
  the explicitly supplied common pool: authoritative, ping check, nonsequential
  allocation, configurable lease (public fixture 1 hour); no udhcpc ARP check by
  default. Pool validation excludes gateway, network and broadcast and rejects
  wrong subnet/range. MBP network planning returns a specific unfinished error.
  Change categories support idempotent reconciliation; repeated input is byte
  identical and produces no changes.
- `portal.c`: binary portal identity, deterministic initial metric ties, >=20%
  improvement sustained by the **same challenger for 60 seconds**, three failed
  completed probe rounds before dropping current selection, and failure bypass
  of optimization hold/margin. Uses supplied complete-path metrics/probe results,
  never an mpp-only eligibility test. Successful probe resets failure count;
  challenger changes/insufficient improvement reset the hold. Role-only MBP/MRP
  changes never request DHCP renew. A real peer selection change requests renewal
  of the existing netifd client, not a second client. MBP foreign-portal conflict
  explicitly requests backhaul-only shutdown and latches until restart.
  These are returned actions, **not already applied operations**.
- `platform.c`, `platform.h`: actual read-only RTM_GETLINK bridge identity query
  and generic-netlink nl80211 GET_WIPHY query. Parses supported mesh iftype and
  requested frequency/regulatory restrictions, including split replies. Bounded,
  timed receives and malformed/truncated attribute rejection. Does not infer
  safe interface concurrency just from an iftype bit. No `iw`, shell, popen,
  child process, BusyBox applet, UCI command or network mutation.
- `main.c`: explicitly read-only development executable, synthetic fixture
  demonstration and native bridge/radio probes. No `run`, `start` or `apply`
  entrypoint. Diagnostic summary intentionally omits caller-controlled IDs,
  address values and credential tokens. No password or PSK member exists in the
  core API: only presence and a non-secret revision token for a platform-owned
  credential. Presence is not a claim that wpad has validated a key.
- Loop protection is explicitly **inactive**. No v7 STP/nftables machinery is
  reintroduced. beta-r1 hardware topology must have no redundant non-mesh links
  until actual loop management is enabled.

## Native milestone integrated with that core

- `native_config.c`, `native.h`: bounded committed UCI file imports through
  **libuci**, not UCI CLI, `uci_load`, saved deltas, writes or commits. Five
  committed inputs (mesh11sd/network/wireless/dhcp/system) are guarded by
  inode, ownership, mode, timestamp and private in-memory content fingerprints.
  Fingerprints and values are never journalled. Same-size, same-clock-tick
  rewrites are detected. Configured fixed channel, htmode, country, LAN device,
  DHCP range/lease and supported naming overrides survive parsing. Explicit
  legacy 2g portal_channel is preserved in a schema-checked STRING field;
  automatic portal channel selection fails explicitly pending migration. Recovered
  group/on-air identifier/key derivation and shared IPv4 seed algorithm are
  implemented, not a new cryptographic protocol. Public synthetic vectors test
  exact derivation; real credentials stay in bounded memory only.
- `factory_fs.c`: real DT/sysfs/proc/pre-jail-copy, nested board.json and committed
  system evidence collection; native ETHTOOL_GPERMADDR on real-root interfaces,
  no shell. Bridge/current MACs cannot substitute for permanent factory identity.
- `native_plan.c`: connects committed inputs, factory collection, actual bridge
  identity, selected PHY evidence and routed-WAN evidence to the tested planner.
  Fixture roots require injected kernel callbacks, never accidental host probes.
  openat2 IN_ROOT/NO_MAGICLINKS bounds intermediate symlinks while allowing real
  sysfs links; unsupported kernels fail closed rather than escape the root.
  board.json and STP evidence are guarded too. The read-only planner requires
  actual STP zero; the integrated peer lifecycle now creates/claims and verifies
  the targeted bridge with STP off before planning. No manual STP-off prerequisite
  remains on that integrated path.
- `platform.c`: native GET_INTERFACE inventory includes netdev and wdev-only
  P2P/NAN identities. GET_WIPHY decodes actual interface-combination grouped
  limits, aggregate maxima, channel count and beacon constraints. The read-only
  `mesh11sd-native --plan` fallback still requires an empty PHY; the peer client
  supplies the stronger desired-AP-plus-mesh proof described below. Foreign
  selected-PHY interfaces are rejected, not deleted or auto-enabled alongside beta.
  RTM_GETROUTE default-route evidence provides an initial peer/MRP indication,
  **not** complete WAN health, GUA or MBP detection.
- `mobility.c`: closed **v8 review item15** overrides legacy v7 policy. Levels
  0–5 are per-node relative motion; portals use rootmode4, static peers rootmode0,
  moving peers rootmode4. Roots1–3 are not emitted. Mobility never changes beacon
  or DTIM. Legacy level6 voice/PA is NOT accepted as motion; a separate traffic
  profile remains future work. Named regression tests enforce these corrections.
  Driver workaround evidence is read-only and does not override this v8 policy.
  Auto detection and queue/AQL/RTS application remain outstanding.
- `runtime_payload.c`: actual fixed-capacity libubox records matching the SDK's
  procd/netifd `wifi-device`, `wifi-iface` and `network-interface` schemas. Peer
  payload includes SAE handoff, configured radio options, recovered supported
  mesh parameters and static ULA management alias. Managed service mode uses a
  stable proto=none LAN and ONE private netifd DHCP client alias; no concurrent
  second DHCP client is started. The original direct-LAN DHCP path remains for
  explicitly already-quiesced service configurations. All additions and
  nested records are checked. Fixed storage avoids secret-bearing reallocations;
  temporary buffers are wiped. No payload is serialized to an artifact or log.
  Unspecified/GUA/ULA-RA MRP profiles explicitly fail. An explicitly selected
  experimental IPv4-only MRP profile now builds a real static gateway, portal
  ULA and ordinary fw4 service-data rules; it never runs a LAN DHCP client.
- `transaction.c`, `runtime.h`: an integrated, tested compare/replace/reload/verify
  state machine. Dry run performs zero backend calls and creates no files. Effect
  tests inject procd/netifd/wpad/kernel transport into the actual native backend,
  never box or router networking.
  Stop/failure/cancellation restores the original owned runtime record; reload or
  metadata failure retains retryable recovery state. Concurrent replacement is
  never overwritten. Private locked journals are atomic, 0600, metadata only.
  Unrecognized crash journals block a fresh owner rather than being overwritten;
  the peer backend now recovers recognized same-configuration crashes. Rollback
  payloads intentionally are not persisted.
- `native_main.c`: `mesh11sd-native --config-check ROOT` and read-only `--plan`.
  Diagnostics are static/redacted and core dumps disabled before reading keys.
  No `--apply`, daemon or autostart entrypoint exists.

## Actual exclusively owned peer integration

- `peer_ubus.c`: real C libubus lookup/invoke transport, bounded validated replies
  and three-second RPC timeouts. No subprocess, ubus CLI or private protocol.
  Actual calls are `service.list/set/set_data/delete`, `network.reload`,
  `network.wireless.status {device}`, `wpa_supplicant.iface_status {name}` and
  named netifd interface `status`. No own WPA/SAE implementation.
- `peer_backend.c`, `peer_blob.c`: private data-only procd service
  `mesh11sd-v8-runtime`, no instances, `autostart:false`, retained noncredential
  owner marker, exclusive lifetime flock, and strict owner/current-data checks
  before writes and after uncertain outcomes. Only this service's data is
  replaced/deleted. **There is no cross-service atomic CAS claim**: a privileged
  noncooperating external writer can still race procd. Changed/foreign owned
  resources fail closed rather than being overwritten.
- `radio_combination.c`, `peer_observe.c`: counts all enabled desired UCI APs,
  including those whose parent radio is currently disabled. Maps active AP
  sections to actual netifd ifnames, proves desired AP+one-mesh grouped limits,
  and checks kernel inventory. A normal supported AP+mesh configuration is
  permitted; unknown mesh/STA/P2P/other selected-PHY resources are not removed.
  Unrelated-PHY baseline identity/frequency/name is checked on verify/cleanup.
  STA_AP_BI_MATCH is correctly not imposed on AP+mesh-only combinations.
- Post-reload verification checks actual mesh PHY/type/frequency, netifd radio
  readiness and mesh section, surviving desired APs, supplicant COMPLETED and
  frequency, LAN DHCP **protocol** and bridge, static/up management alias, bridge
  MAC/STP/master membership and ULA /64 ready after kernel DAD. It does not
  pretend DHCP lease acquisition, another peer or end-to-end reachability has
  been verified. Polls have both monotonic deadlines and finite attempt caps.
- Stop/failure/interrupt deletes the unchanged owned service and reloads committed
  configuration, verifies mesh and management alias disappearance, and preserves
  unrelated resources. Cleanup failure remains explicit and retryable.
- `peer_recover.c`: retained owner + exact reconstructed payload + recognized
  0600 metadata permit same-configuration crash cleanup and reapply. Changed
  configuration or foreign data is preserved as a conflict; no secret rollback
  payload is written to disk. Interrupted metadata is retained with atomic
  rename-no-replace into numbered private files; repeated interruptions do not
  overwrite earlier evidence. Unknown/malformed/overflowing metadata fails closed.
  A crash after service deletion but before reload is handled before planning:
  recognized private metadata permits only a guarded committed reload; stale
  own-name objects must disappear and foreign persistent aliases remain untouched.
- `peer_main.c`: `mesh11sd-peer --run-openwrt-peer` is the **only** enabled-mode
  entrypoint. No arguments only show usage and return 2. Enabled mode requires
  root and OpenWrt identification before opening the live backend. It uses a
  private 0700 runtime directory and 0600 metadata, disables core dumps, monitors
  owner/config changes, and cleans up on SIGTERM/SIGINT. It is not installed,
  auto-enabled or started beside v7; preflight rejects a running v7 service.

**Current integrated behavior:** stock LAN server configuration is handled by
scoped, reversible native role-service suppression below. The owned bridge engine
below similarly removes the manual existing-STP-off requirement. The read-only
planner and intentionally quiesced direct peer API retain their explicit gates;
use the integrated lifecycle for onboarding. General dual-stack routed-portal and MBP runtime apply remain unfinished and
fail explicitly. The new explicitly selected IPv4-only MRP path below is separate
from choosing an unagreed IPv6 policy; it is not a fake successful peer.

Credentials stay in confidential in-memory requests and are never printed or
journalled. Platform handoff uses the existing privileged procd/netifd/wpad
boundary; no new ubus ACL grants are installed. Normal privileged platform
inspection can access platform configuration as before. All test credentials and
identifiers are explicitly public synthetic placeholders.

Relevant SDK reference: wifi-scripts `files/lib/netifd/wireless.uc` service-data
merge and `wireless/mac80211.sh` supported mesh parameters. Matching netifd
`config.c` revision `cbb83a1857407a28a63dc09412a1f209195914ef` consumes procd
network-interface overlays. Correct procd request shape is `name` + `data`;
there is no invented generic `network.wireless.set_config` operation.

## Recovered design, not newly decided policy

Sources: recalled closed-item decisions, the supplied latest Rob conversation,
`../SLICE1.md` and the existing slice implementation. The older review Page was
also read but its original “proposed/open” statuses and STP/nft proposals are
superseded by the closed decisions/latest pure-mesh beta priority.

Preserved: runtime-only automatic configuration, no runtime UCI writes, HWMP
unchanged, no apmond dependency, no live RSSI peer dropping, identity distinct
from MRP/MBP attributes, concurrent recovered routed portals, ULA management,
and no supplicant coordination solely to shorten natural re-peering.

Recovered and implemented: legacy radio band/order selection, explicit fixed
channel, levels 0–5 (level6 explicitly rejected), on-air ID/auto-key derivation, factory fallback chain and
shared IPv4 candidate seed. Automatic channel/migration lifecycle, complete
collision-exclusion across committed networks, full MBP upstream/LAN wiring and
GUA/RA role lifecycle still need completion from the agreed design. No invented
replacement algorithm or new policy decision was substituted for those gaps.

## apmond lessons cross-referenced, read-only

Narrow references in `/workspace/apmond-merge/`:

- `src/lib/factory_mac.c` and `tests/test_apmon.c:793-903`: exact shared identity
  chain, permanence versus rewritten current MAC, DT priority and jail visibility.
  Reimplemented as mesh11sd policy with synthetic equivalents of the ordering
  fixtures. No monitoring, collector, signing, key-derivation or apmond library
  was imported. Native collectors must use mesh11sd's own pre-jail DT copy if
  required, not depend on `/tmp/apmond`.
- `src/apmond/config.c:59-103` and `src/apmond/main.c:140-210`: small modules,
  explicit initialization failure, validated config before starting work,
  monotonic timing, and cleanup on normal stop. The new core makes failed
  plans/decoders transactional and tests failure cases; logging never echoes
  rejected values. The peer client now owns foreground lifetime and cleanup;
  init/package lifecycle is still future work.
- `openwrt/apmond/files/apmond.init`: procd foreground ownership, fail-closed
  privilege setup, pre-jail DT copy, reload triggers. Do not blindly copy its
  capabilities: mesh11sd requires its own audited native networking permissions.
- `openwrt/apmond/Makefile`: matching SDK package machinery, package version
  propagation and a single actual source-of-truth build. Important packaging
  trap: default_postinst can start a service and procd can retain old code on an
  upgrade. Our eventual package must explicitly control start/upgrade behaviour.
  No init/package file that could start this incomplete core was installed or
  emitted. Scratch builds do not invent a clean-tree build_commit.

No apmond reference file was modified. No runtime dependency was added.

## Reproduce

On the box, from `/workspace/mesh11sd-testbed/beta`:

```sh
./build.sh host
./build.sh sanitize
./build.sh analyze
./build.sh sdk-filogic
./build.sh sdk-ath79
./build.sh sdk-mt76x8
```

Host builds use `cc`. The pure core is C11 with `-Wall -Wextra -Werror
-Wpedantic`; native adapters use GNU11 and `-Wall -Wextra -Werror` for base
libubox APIs. Sanitizer builds use ASan + UBSan. `build-native.sh` builds its
host UCI parser and libubus client from the SDK's existing source archives into
`build/vendor/`;
SDK cmake and GNU make are host build tools, not new runtime dependencies. Host tests run only pure fixture code and
native message decoders; they do not configure networking.

Target builds use **only the matching OpenWrt SDKs**, GCC 14.3.0:

- `/workspace/openwrt-sdk/openwrt-sdk-25.12.6-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64`
- `/workspace/openwrt-sdk/openwrt-sdk-25.12.6-ath79-generic_gcc-14.3.0_musl.Linux-x86_64`
- `/workspace/openwrt-sdk/openwrt-sdk-25.12.6-ramips-mt76x8_gcc-14.3.0_musl.Linux-x86_64`

No generic `/tmp/cross` compiler is used. Compiler path/version, SDK provenance
and readelf output are saved beneath `build/sdk-*/`. ABI/loader is verified as
AArch64 musl, big-endian MIPS soft-float musl and little-endian MIPS soft-float
musl respectively. `-static-libgcc` keeps the pure core's only ELF NEEDED entry
`libc.so`. Native binaries additionally need matching base `libuci`, `libubox`
and `libblobmsg_json`; the peer additionally links base `libubus`. The SDK's
netifd Makefile explicitly depends on libuci/libubus/libubox/ubus; jshn brings
libblobmsg-json/libjson-c into base OpenWrt. No additional runtime package is added.
Target binaries are **cross-compiled, not run on hardware**. SDK trees/feeds were
not modified; builds write only into this directory. No `.apk` has been made.

`make test` / `make sanitize` delegate to these same integrated scripts. GNU
make 4.4.1 is now installed. Transient signed official Debian retrieval failed
because SRV lookup selected the box egress relay; the documented fix was a
per-command `Acquire::EnableSrvRecords=false` retry, not mirror hopping or a
persistent apt setting. See
https://forum.cursor.com/t/apt-fails-with-http-500-on-bot-computer-srv-lookup-routes-to-egress-relay-198-18-0-1/176936 .

Logs: `build/host/tests.log`, `build/sanitize/tests.log`.
Both have **144 named checks, zero failures**; each includes a 10,000-buffer deterministic
malformed-netlink corpus. A separate five-case read-only CLI smoke suite plus
fixture JSON/redaction checks also passes. GCC `-fanalyzer` and an `-O0` repeat
pass with the same 144 checks (`build/analyzer.log`, `build/analyzer-tests.log`).
`fixture.h` contains **public synthetic test inputs**,
not deployable credentials or defaults. Source fingerprint of preserved
slice1 files: `preserved-slice1-sources.sha256` (also retained in build/).
Native logs `build/{host,sanitize,analyze}/native-tests.log` each contain **76 checks,
zero failures**, plus native CLI nonmutation/redaction/malformed-input/apply-gate
smoke checks. The reproducible `./build.sh analyze` uses O0 and GCC `-fanalyzer`, warnings as
errors, for the core, both native CLIs and peer tests.
The original slice1 suite was not rerun: its original sources/hashes are preserved.
The original core's 144 checks and 10,000 malformed-message corpus still pass.

Peer logs `build/{host,sanitize,analyze}/peer-tests.log` contain **109 checks,
zero failures**. Together with the preserved 144 + 76 this is **329 named
baseline checks** per host/sanitizer/analyzer run, plus CLI smokes.
The integrated `services-tests.log` adds **68 named checks**, giving **397 total**
(144 +76 +109 +68), including stock LAN handoff, instance binding, real jailed
mount shape, malformed evidence, flat mode calls, absent/explicit restoration,
armed DHCP alias, foreign preservation, compensation and final/held/DOWN crashes. The new peer suite
includes real binary response/combination decoding, 10,000 additional malformed
ubus/interface messages, AP coexistence, grouped limits, ownership contention,
request shape, timeout read-back, interrupts, verification failures, cleanup
retry, foreign netdev/wdev preservation and same-config crash recovery. Tests
execute the real C backend through injected transports; enabled live mode is
never executed. No hardware/APK success is implied by mock transport results.

## Remaining functional work to a working-mesh beta

1. **Implemented peer and explicit IPv4-only MRP handoff** are described below.
   Complete retrieved GUA/RA/MBP policy and runtime WAN-health/role transitions.
   Do not advertise a complete dual-stack or hardware-verified portal.
2. **Owned bridge startup is implemented below.** Explicit committed LAN ports,
   supported options and actual bridge MAC are preserved, WAN excluded. Advanced
   VLAN-filtering/custom device trees and missing committed port mapping remain
   explicit evidence limits, not inferred wiring. Fixed legal channel and selected
   radio work in fixtures; ACS/migration, DFS lifecycle, complete MLO edges and
   queue/AQL/RTS tuning remain.
3. IPv4 MRP now refuses overlap with actual WAN/foreign netifd subnets. Finish
   deterministic alternative subnet selection without restart drift; full WAN
   health/GUA/MBP and failover lifecycle remain. Unspecified IPv6 profiles fail
   explicitly. Loop protection remains inactive.
4. Wire portal/path metrics and timers to the existing netifd client's renewal and
   ARP pinning; preserve role-only no-renew and cable-island-safe discovery.
5. Add managed reload/reconfiguration, status/LEDs/hooks, controlled procd init and
   matching SDK package integration. Active changed config currently stops and
   restores committed state; changed-config crash recovery is deliberately a
   preserved conflict. No automatic v7 coexistence or postinstall start.
6. Build the actual package with matching SDK, then validate controlled startup,
   mesh joining, AP coexistence, stop/restart/recovery on Rob's revised hardware
   only when authorized. No APK or working mesh has been claimed or produced.

No LLDP/ferry or three-carrier policy expansion was mixed into this peer milestone.

## Native peer role-service milestone (hardware unverified)

`services.c`, `services_dns.c`, `services_state.c`, `services_transaction.c`
are integrated with the opt-in `mesh11sd-peer --run-openwrt-peer` C backend,
not an independent mock contract. Runtime base libraries are unchanged.

- Discover every applicable existing dnsmasq instance from committed pool
  binding (unbound pools apply to every enabled instance), then validate the
  running procd command, bounded generated config, confdir permissions and jail
  mounts. Add only our unique private0600 `no-dhcp-interface=<LAN bridge>` include.
  Do not launch another dnsmasq. Native bounded fixed-argv exec calls the existing
  `/etc/init.d/dnsmasq restart <instance>`; no shell interpolation, user environment,
  captured command output or global restart. Verify a replacement PID with
  unchanged instance definition and preserve unrelated instances/PIDs.
- SIGHUP is NOT used as a fake config reload. Platform-triggered selected-owner
  regeneration is accepted only with unchanged command/jail and the same validated
  confdir. Changed generated config under the same PID is a conflict. Changed
  or missing owned include is detected, never silently repaired by verification.
- Publish mesh-held configuration first. Original LAN DHCP client is torn down
  before a private client can start. Stable force-link proto=none LAN reaches UP,
  then REAL flat `network.interface.<LAN>.set_data` disables ra/dhcpv4/dhcpv6/ndp
  with an owner marker. Since the LAN is UP this emits the notification consumed
  by odhcpd. Verify exact mode read-back AND absence from real `dhcp.ipv6ra`
  and `dhcp.ipv6leases` server listings before starting the ONE private DHCP alias
  and releasing mesh. Client autostart is read back as armed, without pretending
  a DHCP lease exists. Lease loss on the alias cannot flush the stable LAN modes.
  No global odhcpd kill/stop, second RA daemon or competing port53 listener.
- Stop reverses the safe order: hold mesh and verify it gone, persist DOWN intent,
  flush owned data with the actual scoped LAN.down, withdraw only the owned
  procd service and reload committed config, restore finite original mode values,
  remove only our include and restart only selected DNS owners. Originally absent
  keys remain truly absent: `set_data(null)` is NOT deletion in this netifd.
- Mode-only0600 journal contains public names/paths and finite enums, no secret
  config payloads/content hashes. Unknown stored interface data is rejected rather
  than erased by down. Unknown/changed journal, foreign mode/include/config changes
  and changed-config crashes fail closed, preserving evidence. Same-config final,
  held and post-DOWN crash stages recover with ordered compensation. An unknown
  or incomplete `services.pending` remains an explicit recovery blocker.
- procd has no cross-service CAS. Lifetime lock, private names and pre/post checks
  detect conflicts; no claim of atomicity against noncooperating privileged writers.
  This supports a finite peer handoff, not portal election/failover or routing.

Matching source references checked read-only: SDK dnsmasq.init and rc.common;
netifd cbb83a1857407a28a63dc09412a1f209195914ef (proto=none immediate,
interface data flush/UP update and procd data integration); odhcpd
5d7be43f8b9dec0eb47e245cfab81108bb131273 (mode override and server-list semantics);
procd 58eb263d5abe03f8c1280bdfa65a3b052614215d (instance-only restart and jail dump).
Narrow public source copies under `build/references/` are build research only.
Conceptual apmond lessons retained: one owner/lifetime lock, native platform
lifecycle, strict read-back, bounded diagnostics and cleanup of only owned state;
no apmond functionality was imported and its source was not modified.

## Recovered DHCP correction — no new per-portal allocator is required

The earlier “deterministic factory-MAC pool allocation” statement was wrong.
Closed item1 describes a **shared replicated pool** served by authoritative
existing dnsmasq, using its **client hardware MAC** allocation hash and normal
ICMP availability checks. It does NOT partition pools by portal factory MAC.
One-hour leases are the reference; never enable `no-ping` or `dhcp-sequential-ip`.
The accepted disconnected-portal duplicate risk is not a new consensus protocol.
Unknown in-range renewals are ACKed under authoritative semantics; an address
held by another MAC is NAKed. Role flips alone do not renew a peer client.

This is backed by matching dnsmasq2.93 `src/dhcp.c:825–921` (SDBM client-MAC
hash; range placement; exclusion of routers, leases, static reservations and
.0/.255; `do_icmp_ping`; collision epoch), and `src/rfc2131.c:1467–1483` plus
1512 onward (authoritative renewal and conflict checks). The official archive's
SHA256 matches the SDK package exactly:
`0c00d4e5c97c8306e5fb932b348b34269c9c29a0e7df0e8e82958b407092bc19`.
Only narrow public source excerpts were extracted under `build/references/`.
Do not duplicate dnsmasq's allocation/probe engine in mesh11sd.

Read-only v7 reference `src/mesh11sd:7142–7184,10925–10941` describes the
**subnet candidate** seed and exclusions, not a per-portal DHCP lease allocator.
The common transformed `auto_mesh_id` seeds normal MRP subnet candidates; CPE
factory-MAC seeding is a separate role. Avoid subnet octets255/168/167/8/1/0 and
already-used gateway candidates. The beta implements the shared seed/excluded
octets; complete foreign-network collision evidence without restart drift is
still outstanding. Preserve configured pool overrides. Factory-MAC IPv6 hints
remain independent of the bridge/station MAC; closed item3 explicitly corrected
the old DAD diagnosis, so no invented DAD stand-down is added.

Read-only v7 `mbp_dhcp_lan_defaults:7951–7989` is corroborating MBP reference:
upstream DHCP client, DHCPv4/v6 server disabled, ULA-only filtered non-default
low-preference RA; do not import CPE NAT66/PD code into MRP. Closed item2's
foreign-portal backhaul-only latched shutdown still applies. Complete closed
GUA/RA conflict decisions were not recovered from the accessible original
conversation: transcript search indexing returned no source conversation. This
is a retrieval limitation, **not evidence of a missing Rob decision**. Retrieve
that closed excerpt from the original parent session before choosing policy.
No IPv4 pool-mapping decision is being requested from Rob.

## Owned bridge milestone (hardware unverified)

`bridge.h`, `bridge_config.c`, `bridge_observe.c`, `bridge.c` are integrated in
`peer_main.c` before planning and peer/service apply, not a detached framework.

- Read explicit committed LAN `config device type bridge`, wired ports and typed
  supported options through libuci. Guard all source changes. Reject unknown
  bridge options/filtering, foreign aliases, nested bridges, missing Ethernet
  evidence, Wi-Fi ports in the wired list, foreign masters and foreign procd
  claims. Normal DSA LAN ports remain distinct from WAN; classic `eth0.1` versus
  `eth0.2` is accepted only with actual distinct kernel VID/lower-device proof
  under `/proc/net/vlan/`. Parent/WAN or unproven same-root overlap fails closed.
- Publish exclusively owned data-only `mesh11sd-v8-bridge` with real netifd
  `network-device` bridge schema, exact ports/options, STP false, bridge_empty,
  and a private force-link proto=none anchor. This anchor actually claims and
  creates the bridge, unlike relying on a hotplug prepare call. No UCI commit,
  uplink bridging or unrelated bridge STP change. There is no loop protection.
- Preserve an existing observed bridge MAC. For an absent bridge, observe the
  actual netifd/kernel MAC and pin it; never synthesize one from factory/station
  identity. Plan ULA management only after verified bridge readiness and STP0.
  Verify exact wired membership and only independently proven AP members or our
  owned mesh. External MAC/resource drift fails instead of being overwritten.
- Private0600 metadata stores only finite public bridge snapshot/options/MACs
  and recovery phase; no credentials, whole UCI backups or secret hashes.
  Guarded atomic intent precedes effects. Recover recognized bootstrap, pin and
  deletion-before-reload phases; reject unknown/incomplete metadata. An external
  deletion of a committed phase2 service is not silently repaired. Prepublication
  actual-state races are rejected before writing.
- Preserve original role-mode snapshots BEFORE bridge reload, then integrate
  mesh-held peer/service startup. Stop mesh, restore role services, withdraw the
  private bridge overlay and reload committed configuration last. Do not restore
  STP under a live owned mesh or uncompleted role cleanup. Same-process committed
  input changes allow withdrawal of exclusively owned resources, not overwriting
  new foreign configuration. Changed-config crash adoption remains fail-closed.
- As with apmond's ownership lessons, one lifetime lock plus pre/post read-back
  protects private names; procd supplies no cross-service atomic CAS. No such
  atomicity against privileged noncooperating writers is claimed.

Matching netifd `config.c:301–347` and `ubus.c:1274–1313`, bridge hotplug prepare
semantics, and procd service-data nesting were checked from the exact source
revisions listed above. Tests execute actual C request construction, parser,
transaction and lifecycle through injected platform transport. They never
configure the box network. `build/{host,sanitize,analyze}/bridge-tests.log` adds
**145 named bridge checks**, preserving397 and giving **542 total checks** per run,
plus CLI/redaction smoke tests and the existing malformed-input corpora.

The following milestone implements that finite IPv4 routed-portal integration.
Complete the closed GUA excerpt and role-failover runtime wiring before claiming
complete portal behavior. No APK, hardware join or successful live mesh is
claimed. LLDP/ferry and loop policy remain untouched.


## Native IPv4-only MRP milestone (hardware unverified)

Modules: `mrp.h`, `mrp_config.c`, `mrp_payload.c`, `mrp_firewall.c`,
`mrp_dns.c`, `mrp_dns_guard.c`, `mrp_live.c`, integrated with existing runtime,
peer, bridge and service transactions. This is real enabled OpenWrt call code,
tested using injected transport; not a new disconnected mock framework.

### Explicit profile, no invented IPv6 policy

`mesh11sd-peer --run-openwrt-ipv4-mrp` is a NEW explicit experimental entrypoint.
It is not installed, started, automatically selected, or enabled alongside v7.
`--run-openwrt-peer` remains the peer entrypoint. Both require root + OpenWrt.
The type-bound `MESH_MRP_IPV4_ONLY` profile deliberately suppresses LAN RA,
DHCPv6/NDP server modes, while preserving actual-bridge ULA management and adding
the agreed per-factory-subnet portal `::1/64` address with native DAD verification.
It does not disable IPv6 on WAN or invent GUA delegation/NAT66 policy.
`MESH_MRP_UNSPECIFIED`, `MESH_MRP_ULA_RA_UNIMPLEMENTED` and
`MESH_MRP_GUA_UNIMPLEMENTED` cannot build a successful MRP configuration.

The exact original design Page was read again. Its item3 DAD diagnosis and
item14 no-handback proposal are superseded by closed decisions: keep factory
hints; many recovered MRPs may resume automatically; peers independently choose
by corrected airtime metric. The Page contains no authoritative closed GUA/RA
policy to implement. Transcript search again returned no source conversation;
no known original Spod session ID was available. Therefore GUA/RA/MBP remain
explicitly parked, not falsely labelled an undecided Rob policy. No message was
sent to Spod, and no new user design decision was assumed.

### Actual native behavior

- Bounded committed libuci reads identify existing LAN/WAN firewall zones and
  precisely ONE existing dnsmasq owner for the LAN pool. Kernel IPv4 forwarding
  must already be enabled (normal OpenWrt router default); it is verified, not
  globally overwritten. Actual `network.interface.wan status` must prove UP,
  IPv4/default-route/resolver evidence and a separate safe L3 uplink name.
  `network.interface dump` rejects gateway-subnet overlap with WAN or unrelated
  networks. There is no guessed alternative pool or restart-dependent drift.
- Runtime procd `network-interface` data retains stable LAN proto=none and
  uses the existing private alias as **static gateway**, never a DHCP client.
  The normal IPv4 gateway/subnet and preserved bounds/lease enter the actual
  netifd schema. Factory identity, actual bridge MAC and mesh station remain
  distinct. Neither WAN nor its ports are moved into the LAN bridge.
- Add ordinary IPv4 `firewall` service data to the SAME exclusively owned
  procd runtime record: LAN-to-WAN forwarding, source-subnet/uplink-scoped
  masquerade, and LAN-only DNS/DHCP input. Existing fw4's documented ubus data
  consumer compiles it. There are no bespoke nft loop rules, static nft includes,
  bridge-family tables or STP backstops. Remove only this owned data on stop.
- Native C fixed-argv execution calls existing `/sbin/fw4 reload` and
  `/usr/sbin/nft --json list chain inet fw4 <validated-name>`; stdout/stderr are
  never logged. Bounded capture, timeout, private child process group, fixed
  environment and no shell interpolation/popen. Actual kernel NFT expressions,
  not merely comments, must match device, IPv4 source prefix, transport/ports
  and expected verdicts. Unknown constraints/wrong interfaces/verdicts fail.
  Missing-chain absence requires a successful native chain inventory, not an
  assumed absence from a failed command. fw4 rebuilds its normal ruleset;
  no cross-service CAS or preservation of arbitrary unmanaged fw4-table edits
  is claimed.
- Keep mesh held while suppressing original services and bringing up the
  gateway. In the SAME validated dnsmasq owner, atomically replace the unique
  owned0600 suppression include with an authoritative common DHCP range,
  using one owned range tag with preserved bounds and lease; explicit router/DNS options and
  a reserved `.invalid` DNS readiness marker. Use dnsmasq's existing client-MAC
  allocator, ICMP checks, leases and static reservations. No new DHCP/DNS
  process, allocator, `no-ping`, sequential mode or factory pool partition.
- Restart only the selected dnsmasq instance and verify its replacement PID
  with unchanged command/jail/mount definition. Audit effective main/include
  configuration, bounded recursion, to reject hidden no-ping/sequential/range
  overrides without printing or saving its contents. Native verification checks
  actual kernel gateway address/netmask, IPv4 default route, ip_forward, the
  selected PID's real UDP67 socket inode, and an actual gateway DNS A response
  to the owned marker. Only then release mesh. Unrelated DNS owners remain up.
- Stop/failure/signal holds mesh first, returns DNS to suppression (stops DHCP),
  flushes owned LAN mode data, withdraws gateway/procd data and reloads netifd/fw4,
  proves owned rule absence, restores original modes and DNS include state,
  and restores the bridge last. Same-process committed-input changes still
  permit owned withdrawal. Foreign include/config/resources are not overwritten.
- Version2 private0600 service metadata adds only public pool fields and a
  finite DNS transition intent. Recognized same-profile active and interrupted
  suppression crashes are recoverable without storing credential payloads.
  A different-profile restart is rejected rather than reinterpreting old
  portal state. Atomic include temporary files are dot-prefixed so dnsmasq
  cannot load a half-completed replacement. Unknown pending files fail closed.

### Deliberate finite limits and next concrete step

An explicitly disabled DHCP override, custom unsupported IPv4 pool options,
multiple local dnsmasq owners, or an owner also serving another enabled pool is
rejected, not silently changed (authoritative is instance-wide). Standard stock
OpenWrt LAN/WAN is the first supported case; normal default config was checked
against actual SDK sources, not an impossible invented platform API. Advanced
bridge/VLAN limits from the previous milestone still apply.

Local configuration verification is NOT a successful DHCP client exchange,
Internet DNS/forwarding test, joined mesh, multi-portal lease/failover proof or
hardware validation. Hardware tests have not run. WAN health/role transitions,
portal discovery/ARP pinning/renewal wiring, alternative collision-free subnet
selection and dual-stack/MBP remain. Preserve closed item14's multiple recovered
MRPs and port/review legacy `calc_airtime_metric()` rather than trusting raw
kernel/driver airtime metrics (known large erroneous offsets); retain the
20%/60s/three-failed-probes selection policy.

Next: integrate managed daemon role/readiness/status/reload behavior, then a
non-auto-enabled procd service and matching SDK package. First experimental APK
must honestly identify this explicit IPv4-only profile and inactive loop
protection; do not claim a complete mesh until authorized hardware validation.
No APK or service/package installation was made here.

References: exact SDK firewall4 revision
`b6e5157527d361f99ad52eaa6da273cb0f2dfd59` (`fw4.uc` read_ubus/load/parse_rule/
parse_nat; `templates/rule.uc` IPv4 family/transport/input matches; redirect
template's oifname and masquerade; native fw4 entrypoint),
matching SDK dnsmasq.init `dhcp_add`/instance restart, dnsmasq2.93 allocation
references above, and router DEFAULT_PACKAGES. Existing firewall4 already
requires nftables-json/ucode; this milestone adds NO runtime package, new ucode
program, own crypto, or libnftables dependency. Only existing base components
and C/libuci/libubus/libubox are used. SDK feeds/trees are not modified.

### Verified milestone6 results

`./build.sh host`, `./build.sh sanitize` and `./build.sh analyze` each pass
**631 named checks, zero failures**: preserved542 (144 core +76 native +109 peer
+68 services +145 bridge) plus **89 MRP checks** in `tests/test_mrp.c`. New cases
cover actual netifd/fw4 request shapes, DNS-range/tag construction, unsupported
IPv6 profiles, foreign subnets/config/includes, hidden DHCP overrides, native
NFT expression proof, DNS packet/socket parsing, failure compensation, repeat
apply, active/interrupted suppression recovery and profile mismatch. Bounded
10,000-packet mutation and truncation tests exercise the DNS reply decoder.
Whitespace/comment spellings, quoted/wildcard exclusions, foreign-only interface
whitelists and ignore tags cannot silently turn off the owned LAN DHCP service.
Kernel input-rule verification also requires the actual IPv4-family condition
from fw4's matching `rule.uc` template, preventing silent IPv6 broadening.
The selected DNS instance is regenerated while still suppressed and its
effective config audited BEFORE any authoritative allocator restart; a hidden
no-ping fixture proves there is no unsafe server activation window.
Canonical LAN/loopback whitelists and WAN exclusions remain accepted. Complex
quoted include/interface paths and dynamic config scripts fail closed.
CLI smoke tests remain nonmutating; live entrypoints were never executed.

GCC analyzer runs with warnings as errors. It exposed a descriptor-tracking
ambiguity in the native child runner; explicit pipe/write ownership now makes
all cleanup paths verifiable without suppressing warnings. ASan/UBSan pass.
`./build.sh sdk-filogic`, `./build.sh sdk-ath79`, and
`./build.sh sdk-mt76x8` all pass using the matching25.12.6 SDKs. Final target
ELF headers/loaders/dependencies are recorded in each SDK build directory; the
peer needs only existing base libuci/libblobmsg_json/libubus/libubox/libc.

All eight `preserved-slice1-sources.sha256` entries still match. The original27
suite was not rerun or edited. Source/result manifests and private0600 archive
are retained as `build/milestone6-*` and `backups/milestone6-source-*.tgz`.

## Independent audit corrective checkpoint (milestone6a)

At checkpoint6a, further portal/daemon work was paused. Unintegrated metric/LLDP sources were
parked in `pending-m7/`; no part is linked or counted as a delivered feature.
The immutable milestone6 source/archive remains available to the reviewer.

The audit correctly found two BLOCKING defects missed by milestone6's fixture
assertions: dnsmasq rejects two `set:` tags in one DHCP range; and an explicit
netifd LAN down clears autostart, which an unchanged-auto reload does not undo.
Those earlier tests were insufficient evidence of a usable portal.

Corrections:
- `mrp_dns.c` emits one `set:mesh11sd_v8` tag and options referring to that same
  tag. Conditional reservations depending on the former generated LAN tag fail
  explicitly instead of being silently reinterpreted. Unconditional static
  reservations remain supported. The old LAN-tag-preservation claim is withdrawn.
- `tests/run_peer.py` runs the generated server and suppression files through a
  REAL `dnsmasq --test --port=0 -C <fixture>` and also requires the former two-tag
  config to fail with the parser's specific error. The host checker is built
  from the SHA-verified exact2.93 upstream archive pinned by the25.12.6 SDK; it
  is not the reviewer's UNKNOWN-version HEAD binary, nor a target firmware binary.
  OpenWrt patches are not applied to this upstream grammar oracle. No listener
  or DHCP service is started by these syntax-only checks. Build proof is in
  `build/dnsmasq-syntax-{source,version,build}.log`.
- Final service restoration explicitly invokes scoped LAN up ONLY if the saved
  prior state was UP, otherwise down; waits for the actual up/pending state;
  restores modes/DNS and verifies the state again before deleting its journal.
  This same path handles clean stop, compensation and crash-after-down recovery.
  The fixture now models manual-down/autostart latching across reload; prior-DOWN
  restoration never issues up. Matching netifd pin:
  `cbb83a1857407a28a63dc09412a1f209195914ef`, interface.c down/up/reload.
- Selected dnsmasq restart return failures propagate even if a replacement PID
  appeared. New PID/definition/generated-config evidence must remain stable for
  nine reads250ms apart (two seconds). Asynchronous platform PID adoption also
  requires stability; transient/crashloop PIDs fail. This is bounded startup
  stability evidence, not a promise the process cannot fail later.
- Scoped odhcpd suppression now checks `ipv4leases` as well as RA/DHCPv6 lists.
  An empty device entry still means active MODE_SERVER. Only the exact optional
  METHOD_NOT_FOUND status is accepted for a non-maindhcp/ipv6-only build; timeouts
  and other errors fail. Committed `maindhcp` evidence is parsed and tested. Exact
  SDK odhcpd pin5d7be43f8b9dec0eb47e245cfab81108bb131273 registers ipv4leases only
  under DHCPV4_SUPPORT; a full build always exposes it regardless of maindhcp.
- Authoritative remains instance-wide: unrelated enabled pools and hidden foreign
  ranges are rejected before allocator activation. No competing DHCP instance is
  started. The UDP67 inode check proves a selected process socket ONLY, never
  bridge-specific DHCP packet delivery. A real DHCP exchange and hardware tests
  are still outstanding. No APK or full portal success is claimed.

Corrective checkpoint verification: `./build.sh host`, `./build.sh sanitize`,
and `./build.sh analyze` each pass **650 named C checks, zero failures**, plus
**three actual pinned dnsmasq2.93 syntax-parser cases**. Counts:144 core +76
native +109 peer +86 services +145 bridge +90 MRP. Both generated profiles pass;
the old two-set-tag regression fails as expected. `./build.sh sdk-filogic`,
`./build.sh sdk-ath79`, and `./build.sh sdk-mt76x8` all pass with matching25.12.6
SDKs. No new runtime dependency is added. All eight original slice1 hashes match.
Original milestone6 and reviewer snapshot are unchanged. Source/result records:
`build/milestone6a-*`; private corrective archive: `backups/milestone6a-source-*`.


## Milestone7 — integrated native pure-mesh portal runtime

This continues checkpoint6a, without reverting either independent-audit fix.
`pending-m7/` is the old parked checkpoint, NOT the current implementation.
The top-level files below are compiled into the existing explicitly opt-in
`mesh11sd-peer` program. Nothing was started on the box or a live router.

### Actual implemented operations

- `airtime.c/.h`: v7's custom integer airtime calculation and driver-rate
  workaround, with packet/loss/retry DELTAS over a sampling window and the
  `round(raw * hops^1.6)` adjustment. Kernel raw airtime metrics are ignored.
  Named vectors cover54Mbps/0% =152,54Mbps/10% =168,90% clamp =1503,
  hop2 =461, hop3 =882, real RX108Mbps =76 and conservative20Mbps =410.
  First/reset/stale windows are explicitly unavailable, not zero/best metrics.
  Missing station counters or all-zero/missing rates cannot produce a successful
  measured metric. Consistent with the old strong-link fallback comment, RX
  fallback requires a supplied threshold and observed stronger signal; unset
  thresholds deliberately use conservative20Mbps for low reported TX rates.
  This influences rate choice only: NO live RSSI peer dropping or HWMP changes.
- `platform.c`: actual native nl80211 `GET_MPATH` for the advertised mesh station,
  then `GET_STATION` for its actual next hop. Decode nested16/32-bit bitrates,
  packets/failed/retries, signed average signal, ESTAB, active path and hop count.
  Validate message commands, lengths, duplicates, interface/destination/next-hop
  identities. No `iw`, MPP-membership requirement or raw driver metric fallback.
- `portal_lldp.c`: bounded portal-only LLDP codec/cache. The chassis identifies
  factory MAC, port identifies mesh-station MAC, Ethernet source/description
  identify ACTUAL bridge MAC. Description contains portal ULA, role ATTRIBUTE,
  shared IPv4 gateway and actual observed IPv6 link-local; never mesh ID/key.
  Standard5second hello/15second lifetime, zero-TTL withdrawal. Reject malformed
  TLVs/fields, foreign shared48/gateway, source mismatch and same-ULA/different-
  factory conflict. No ferry or nonmesh mechanism is activated.
- `portal_live.c`: actual AF_PACKET receive/send on the selected mesh interface.
  Local identity comes from the existing ULA plan and observed interface/bridge
  identities; requires a DAD-ready actual bridge link-local address. The native
  probe sends a correlated ICMPv6 echo directly through that selected mesh
  interface to the advertised bridge MAC/link-local, and accepts a reply ONLY
  on that interface with matching L2/IP identities, nonce, checksum and echo ID.
  This avoids claiming WAN/another LAN path was a successful mesh-path probe.
  The nonce is correlation, not a new authentication/crypto implementation;
  mesh admission remains wpad/SAE's job. No invented shared48 on-link route.
- `portal_runtime.c` reuses `mesh_portal_update()` for20%/60second/three-qualified-
  failure selection. A *successful* path probe with unavailable/reset telemetry
  DEFERs selection comparison; it does not manufacture a cost or count as a
  failed probe. Freshness is checked again AFTER elapsed native probe time,
  including for candidates measured earlier in a slow round. Local MRPs stay
  independent and never stand down because another restored MRP is heard.
  MRP/MBP changes for the SAME remote identity do not renew the DHCP client.
- `portal_neighbor.c`: real scoped `RTM_GETNEIGH/NEWNEIGH/DELNEIGH` on only the
  configured IPv4 gateway of the owned bridge. ACK/request construction and
  response parsing are fixture-tested. An existing configured/permanent,
  noarp, flagged or foreign-type entry is NOT taken over. Snapshot supported
  dynamic prior state; intent/read-back on replacement; restore only recorded
  owned state. Detect live external mutation instead of overwriting it.
  This pins THIS NODE's IPv4 gateway neighbor only. It does NOT filter gateway
  ARP replies for bridged AP/LAN clients; no legacy bridge nft ARP rules were
  copied and no all-clients pinning claim is made.
- `portal_state.c`: fixed96byte, private0600 atomic public-identity journal,
  with before-mutation install AND restore phases and file-stamp guards.
  Test crashes before/after a successful pin and restoration, including ACK
  loss, and normal dynamic NUD aging. Kernel-flushed cache state is not blindly
  recreated. Clean restart may rebind a new bridge ifindex with no owned pin;
  interrupted old-device ENODEV/absent-neighbor recovery never writes onto a
  recycled index. A cleanly released public history can rebind an explicitly
  changed common gateway or reset after a mesh-group change without touching
  old network objects. An active interrupted pin with changed group/gateway
  instead fails explicitly; it is not silently reassigned to the new config.
  An extant old foreign entry blocks recovery. This is an
  exclusive owner/read-before-write protocol, NOT atomic cross-service CAS.
- The foreground program ALWAYS uses the existing backed-up, controlled LAN
  service transition and private client alias, including on already-prepared
  LANs. It cannot silently renew a foreign/pre-existing LAN client. The older
  low-level unmanaged fixture backend is not a supported portal-runtime mode.
- Actual client renewal is a scoped, empty netifd
  `network.interface.mesh11sd_v8_client renew` RPC after the OWN DHCP alias is
  observed on the intended bridge, autostart and up/pending. No auxiliary
  `udhcpc`, global PID signalling or destructive network restart. A public
  last-notified identity persists until reboot to avoid role/restart storms.
  Journal request intent before sending once: timeout/crash is explicitly
  uncertain and NOT resent every polling cycle. Ubus acceptance does NOT prove
  a completed DHCP exchange or exactly-once delivery across a crash.
- `peer_main.c` ties these components into the existing managed peer/IPv4-MRP
  lifecycle: native observations/announcements, selection, ARP pin and actual
  renew RPC, fixed public diagnostics including raw/adjusted/next-hop evidence,
  graceful SIGINT/SIGTERM, and explicit SIGHUP stop/restore/replan/start.
  ARP release precedes owned peer/service/bridge withdrawal. Cleanup conflicts
  prevent restart; an ARP ownership conflict retains the bridge anchor for
  explicit recovery rather than hiding failed cleanup. Owned names remain
  fixed, so reload does not create accumulating dynamic instances. Install
  remains inactive; no enabled procd init/service or package is installed.
  `--version` is nonmutating; no arguments still perform NO action. Existing
  `mesh11sd-native --plan` / `--config-check ROOT` and public fixture planner
  remain the nonmutating validation interfaces, not fake successful apply.

### Exact platform evidence checked

- SDK25.12.6 mac80211/backports6.18.55 public archive SHA256:
  `ca3666d1b89a0399747fb9b9fd7b0a4d0a1568548198d800acaf7ca0e8659979`.
  Downloaded into this scratch build cache; shared SDKs/feeds untouched.
  Narrow reference copies are `build/references/backports-*`.
  `nl80211.c` sends NEW_MPATH/NEW_STATION replies for GET requests and emits
  u8 hop counts. `net/mac80211/cfg.c:2606-2669` actually populates next-hop,
  active flags AND hop_count. This is an implemented mac80211 API, not an
  imagined universal driver capability. Unsupported drivers fail clearly.
- Read-only v7 reference `/workspace/mesh11sd/src/mesh11sd:5989-6111` for the
  custom algorithm;3091-3153 for the OLD renewal effects. No script was ported
  wholesale. The new renewal intentionally fixes same-identity role flaps.
- Exact netifd pin `cbb83a1857407a28a63dc09412a1f209195914ef`:
  `ubus.c:480-489` always returns RPC0 after invoking renew;
  `interface.c:1182-1187` rejects DOWN/TEARDOWN. SDK's actual
  `lib/netifd/proto/dhcp.sh:128-132` signals the existing DHCP client SIGUSR1.
  Therefore readiness is checked and only request acceptance is reported.
- Recalled closed items7/14 and the supplied Rob follow-up remain authoritative:
  connect-time RSSI, independent recovered MRPs, custom metric/counter deltas,
 20%/60s/3fail, identity-only renewal. The old review Page was read again; its
  older STP/live-drop/no-handback proposals do not override the closed review.
  Existing corrected root4 portal/moving, root0 static, level6 reject and ULA
  identity tests remain untouched.

### Remaining limits / shortest next steps

This is a substantive native portal runtime, NOT yet a complete automatic mesh
product, installed APK or hardware proof. Startup still uses the explicit peer
or IPv4-only-MRP profile. **Meaningful source-bound WAN health and automatic
MRP/peer role transitions/recovery are NOT implemented by this milestone**;
current MRP readiness checks prove configured route/local services, not Internet
reachability. Do not interpret an LLDP MRP claim as verified Internet health.
Next integrate the recovered WAN-role policy with a real controlled upstream
probe and ordered existing role backends, then the disabled-by-default procd
entrypoint/status endpoint and matching SDK package build. The foreground
program has public periodic status, not yet a queryable managed status API.

Native gateway pinning currently affects the node's own L3 traffic only;
bridged AP/LAN client ARP-selection behavior needs the agreed native mechanism,
not a claim that a route-neighbor entry governs bridged frames. Own-netifd
renewal is request-tested, not a verified on-wire DHCP exchange.

No GUA/RA/DHCPv6/MBP backend or collision policy was inferred from old proposals.
Those retrieved-agreement gaps remain explicit, not attributed to an unmade Rob
choice. No nonmesh/ferry links, loop management, MLO catch-all or automatic
channel/collision algorithm was added. **Loop protection is inactive**, not
provided by the intentionally STP-off mesh-used bridge. A slow large portal set
still has bounded sequential probes; asynchronous scaling is not claimed.
Controlled revised-hardware startup/traffic/renewal/failure/restart validation
remains necessary before claiming a working mesh or beta APK.


### Milestone7 verified checkpoint

Completed 2026-11-07 15:25:04 GMT (Europe/London).
`./build.sh host`, `./build.sh sanitize`, and `./build.sh analyze` each
pass **763 C checks (all prior650 +113 portal-runtime), zero failures**, plus
**three real pinned upstream dnsmasq2.93 syntax-parser cases**. Two separate
10000-case bounded malformed native-station/LLDP mutation corpora execute
inside each portal suite. No live socket/network apply was exercised.
`./build.sh sdk-filogic`, `./build.sh sdk-ath79`, and
`./build.sh sdk-mt76x8` all pass using matching OpenWrt25.12.6 SDKs. Native
math links into base musl: final target DT_NEEDED remains exactly libuci,
libblobmsg_json, libubus, libubox and libc; **no new runtime library/package**.
All eight original slice1 hashes match; original27 suite was not rerun.
All nine corrective6a source/test/build files checked against that checkpoint
remain byte-identical. Results: `build/milestone7-results.json`; final source
manifest: `build/milestone7-sources.sha256`; private0600 source snapshot: `backups/milestone7-source-20261107-152504.tgz`.


## Milestone8 — source-bound boot role, real queryable status, opt-in service

This is **not a validated working mesh or v7 replacement**. No live host/node
networking was applied and no init service was installed. No APK was produced:
matching-SDK packaging reached a real host-prerequisite blocker described below.

### Native integrated WAN evidence and finite role policy

`wan.c/.h` reads the exact stamped committed network file through the existing
bounded/libuci snapshot path. It requires a direct committed WAN `device`, an
unchanged snapshot, matching netifd WAN `device`/`l3_device`/protocol, one IPv4
source/prefix, own default-route evidence and configured WAN IPv4 DNS servers.
The source prefix must not overlap the common mesh/client pool. Loopback,
link-local, local-source and mesh-pool resolver targets cannot qualify. Invalid
DNS text, wrong scalar types and changed committed inputs are explicit errors.
Missing/disabled WAN is absent; unsupported PPP/L3 mappings or IPv6-only DNS are
partial, not invented healthy uplinks. No UCI writes or credentials are logged.

`wan_live.c` makes actual Linux/OpenWrt calls when explicitly enabled:

* Confirm the source is assigned to that exact UP nonloopback WAN device; refuse
  an enslaved WAN so it cannot accidentally be the owned LAN/mesh bridge.
* Construct a native `RTM_GETROUTE` query for the exact resolver, WAN source and
  output interface. Reject mismatching output/destination/source, local or
  blackhole routes and unsupported multipath/via results.
* Use a UDP socket bound both to the committed WAN device (`SO_BINDTODEVICE`)
  and its assigned source. Query only a DNS endpoint supplied by that WAN's
  netifd evidence. Match the connected response, random transaction identifier,
  root-NS question, bounded compression and a valid root NS answer. No arbitrary
  public test address, `ping`, `iw`, shell interpolation or alternate-portal
  reachability is used.

The honest health name is **`configured_dns_qualified`**, with
`internet_verified=false`. A cached answer from an upstream router proves a
source-bound configured-DNS path, NOT general Internet access. The full agreed
WAN target/hysteresis/reentry policy was not recovered; this criterion is the
explicit constrained first-beta boot qualification, not a fabricated policy.

`peer_main.c` connects this evidence to the existing planner and actual managed
bridge/peer/MRP lifecycle:

* `--run-openwrt-auto-ipv4`: choose peer at boot if absent/partial/unreachable;
  choose local IPv4 MRP only when qualified; malformed evidence refuses startup.
* `--run-openwrt-peer`: a safe explicit fixed-peer override, not a fake portal.
* `--run-openwrt-ipv4-mrp`: demands the same own-WAN qualification.
* Existing committed `portal_detect=0` forced-MRP settings are never silently
  changed to peer. Conflicting forced/unsupported portal modes fail before LAN
  transition. Existing planner modes1/2 use the confirmed startup role callback.
* Multiple independently qualified boot MRPs remain possible; no exclusive
  portal election or suppression of a restored MRP has been invented.
* During a session the role is frozen. An active peer does NOT opportunistically
  promote when WAN returns. A running MRP requalifies its own WAN on portal
  rounds; lost/invalid qualification causes the existing ordered withdrawal and
  exit, not further healthy advertisements or a speculative role flip. There
  is no guessed WAN flap hysteresis or automatic stable reentry. Explicit HUP
  cleanly restores/replans/requalifies; restart is also available.
* Existing20%/60s/3-failed-peer-probe selection and identity-only node-client
  renewal are unchanged. WAN qualification failures are NOT passed off as that
  separate agreed peer-selection hysteresis.

### Actual read-only status and thin procd lifecycle

`status.c/.h` registers **`mesh11sd.v8` / `status`** through real libubus, under
one owned service object. The provider has no mutation methods. The active
foreground loop polls its connection and answers typed, fixed-schema status;
status requests do not accelerate network verification ticks. Failure of the
status connection causes guarded withdrawal rather than an unqueryable daemon.
A second provider cannot replace the namespace; closing the provider releases
only its own object. Every clean HUP/restart uses a fresh object type ID.

`mesh11sd-peer --status` (also `mesh11sd-peer status --json`) queries that API and prints the same whitelisted JSON.
The packaged binary name is `/usr/sbin/mesh11sd-v8`; it does not replace v7's
binary. Status includes phase, role, auto-at-boot/fixed policy, WAN qualification
and public activity flags. It explicitly reports hardware/Internet validation,
loop protection, bridged-client portal pinning and IPv6 RA support as false.
There are no credential/on-air-ID fields, no raw UCI payload or arbitrary reply
string passed into stdout. The initial version/default invocation remains safe
and nonmutating. The status handler is foreground/serial: a query during a long
blocking startup/restore operation can time out; this is not an asynchronous
all-phases status guarantee. Root-system-ubus caller permissions are required;
no broad extra system ACL was installed.

`packaging/files/mesh11sd-v8.init` and the separate
`mesh11sd_v8_runtime` config are experimental opt-in only (`enabled=0`). The
pinned SDK APK installer exports `pkgname` and calls `default_postinst`, which
runs init `enable`/`start` BEFORE a package-specific postinst. Both operations
therefore explicitly refuse installation/image/upgrade context, even if an old
runtime config says enabled. A late postinst stop/disable is not relied on.
Manual activation remains possible only after explicit configuration. The
existing C preflight also refuses a running v7 `mesh11sd` service; it never stops
v7 automatically.

The init script selects only a fixed whitelist of native arguments, opens one
`main` procd instance, suppresses stdout, forwards only redacted native stderr
and permits120s TERM cleanup. There is intentionally no respawn/requalification
loop. Reload sends one owned numeric HUP; disabling then reloading requests stop.
Changing the runtime `mode` requires explicit stop/start (HUP keeps argv).
It uses standard pinned rc.common/procd operations, not copied apmond features,
extra BusyBox applets, ucode, secret command arguments or runtime UCI commits.

### Client network limitation — node neighbor pin is not bridge policy

The narrow read-only v7 reference `src/mesh11sd:4675–4848` confirms its separate
mesh-ingress bridge ARP filtering: accept only the selected portal mesh-station
and actual bridge identities for the common gateway; filter both replies and
requests claiming that gateway. MBP upstream-router exceptions differ. This is
NOT what a node-local rtnetlink neighbor pin does. Nothing in milestone8 claims
that pin controls bridged AP/LAN clients.

That client-L2 mechanism is still required for specified independent best-portal
selection/failover with multiple MRPs. It was not silently replaced by node
renewal or a log message. No v7 STP/nft loop rules were reinstated. The SDK defines
bridge nft support as the separate `kmod-nft-bridge` capability; SDK availability
is NOT proof of its presence in every actual firmware image, so it cannot be
assumed always-present or added as an unnoticed runtime dependency. A first
hardware test should use one MRP; even that client DHCP/DNS/NAT path has not yet
been verified on wire. Concurrent MRPs remain permitted by boot-role code, but
bridged-client best-portal enforcement is unsupported and explicitly visible in
status. GUA/RA/DHCPv6/local-MBP, ACS/collision/mobility edge policies and loop
management remain the previously documented gaps; no guesses were added.

### Packaging checkpoint and actual verification

`packaging/Makefile`, `packaging/build-package.py` and the init/config files
provide the local experimental SDK package path. `0.0.0_pre8-r0` is only its
private packaging identifier, NOT a public release/version/tag. The builder
copies the exact25.12.6 SDK into `build/package-sdk-filogic`, excludes repository
metadata and other builds' signing keys, holds a private packaging lock and puts
the package in its own `package/` namespace, never shared SDK feeds/configs. It
stages only source, uses the actual SDK target compiler/base libraries and
expects an actual SDK-produced APK before writing any artifact-success record.
This recipe has NOT yet completed a package build and is not advertised verified.

The first local SDK attempt exposed/corrected two builder details: SDK package
payloads require umask022 (the enclosing private workspace is0700), and the SDK
must discover real host tools through its own prerequisite logic rather than a
manually prepended staging host/bin PATH (which creates self-referential links).
GNU Make4.4.1 is now present and passes. The remaining real prerequisites are
box-side g++, ncurses headers/library, rsync, GNU awk, bzip2 and file. Installing
those official Debian packages was simulated (zero upgrades/removals), but all
archive requests returned HTTP500 from relay198.18.0.1. Nothing was installed.
No FORCE bypass, unsigned binary, mirror/DNS rerouting or host-network change
was used. Packaging remains blocked; no APK exists and nothing was installed.

Mandatory unblock research read memory, the official forum and X. The public
forum documents the same relay/SRV issue:
https://forum.cursor.com/t/apt-fails-with-http-500-on-bot-computer-srv-lookup-routes-to-egress-relay-198-18-0-1/176936
No instruction from those outside sources was treated as permission to change
network routing or circumvent the box's network failure. Resume the same private
SDK recipe after supported official host-tool fetching is available.

Exact verification commands/results are recorded in `build/milestone8-results.json`.
The preserved763 C checks are joined by76 WAN and25 status checks (**864 total**),
plus the same three actual pinned dnsmasq2.93 parser cases. WAN/status add20k
malformed DNS and20k blob mutations to the existing20k portal telemetry/LLDP
mutations per host-mode run. `python3 tests/test_init.py` adds16 shell request
fixtures; these do NOT claim real procd execution.

The status test additionally runs a real pinned ubusd on a private AF_UNIX
socket, not a mock broker or the system bus: genuine registration/reply/collision,
updated state, close/reopen and unrelated-object preservation are exercised.
Only one synthetic ACL file is chowned root, as the real ubusd requires; broker
and client stay unprivileged and the temporary tree is removed. The SDK's host
fakeroot was checked and cannot intercept this host's new-glibc stat metadata;
it is not used to fake a successful status permission check. The test server's
unused compiled DEFAULT socket directory is scratch-only, so even its startup
mkdir does not touch host `/run` or system ubus. No AF_INET WAN probe, host
interface change or live OpenWrt apply mode was executed on the box.

Final milestone8 verification completed 2026-11-07 16:01:55 GMT
(Europe/London): `./build.sh host`, `./build.sh sanitize`, `./build.sh analyze`,
`./build.sh sdk-filogic`, `./build.sh sdk-ath79`, `./build.sh sdk-mt76x8` all exited0.
Every host-mode run passed864 C checks plus3 real dnsmasq parser cases and the
actual private-ubusd lifecycle. `python3 tests/test_init.py` passed16 fixtures.
One fanalyzer finding in the negative TEST (unchecked fixture field before
mutation) was fixed with an explicit null guard and all commands rerun; no
warning suppression or protected corrective6a code change was made. All8 slice1
hashes and all9 corrective6a files remain byte-identical; original27 suite was
not rerun. Results: `build/milestone8-results.json`; source manifest:
`build/milestone8-sources.sha256`; private0600 backup: `backups/milestone8-source-20261107-160155.tgz`.

Shortest remaining steps: restore supported official box host-tool fetching,
finish/inspect an actual matching-SDK APK (payload, permissions, dependency and
installer guards); validate controlled single-MRP pure802.11s startup and real
client DHCP/DNS/NAT on Rob's revised hardware; implement or capability-gate the
separate bridged-client ARP policy for multiple MRPs. Recover exact WAN dynamic
hysteresis/reentry and IPv6/MBP policy before claiming those modes complete.
