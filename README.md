# mesh11sd experimental C layout (0.0.0_pre8-r0)

Unpublished working tree for the experimental C implementation. The packaging
identifier is `0.0.0_pre8-r0`. There is no 8.0.0beta tag, no release tarball,
and no hardware validation in this tree.

The shell daemon documented for 7.x remains on branch `7.3.2beta` at
`4988d06ab817213c57b9cd39b4beb3d8a2731f80`. Use that branch for the 7.x
manual, default UCI file, and image instructions. This tree does not carry
those instructions forward.

## Layout

| Path | What |
|---|---|
| `src/lib` | Shared C: planner, native OpenWrt adapters, peer/MRP/bridge/portal/status |
| `src/mesh11sd` | Entry points: `main.c` (read-only planner), `native_main.c`, `peer_main.c` |
| `tests` | Host fixtures. Several suites `#include` another suite and compile with the library sources |
| `openwrt/mesh11sd` | OpenWrt 25.12+ apk recipe and the disabled runtime config |
| `scripts` | Parent-box SDK driver and the full host/sanitizer/analyzer/SDK build scripts |
| `CMakeLists.txt` | Host planner checks, and the daemon build the package recipe invokes |
| `.build_commit` | `$Format:%h$` for a future `git archive` export-subst. It is not a commit id |

`sha256.c` lives in `src/lib`. Relative to the scratch handoff, its include is
`"sha256.h"` and the other two former `../slice1/sha256.h` includes match it.
Later edits add file headers and tab indentation. They do not change the
normalized C tokens. The packaging identifier stays `0.0.0_pre8-r0`. A local
branch name is not a version and does not change that identifier.

## Source style

C and headers under `src/` and `tests/` use tabs for indentation. A line may
keep up to three spaces after those tabs when a continuation needs a column
that is not a tab stop. Python, YAML, and Markdown stay space-indented: a tab
is a Python syntax error, is invalid in YAML indentation, and breaks Markdown
layout. `tests/test_c_style.py` checks the headers, the indent rule, and the
normalized token digest. `sha256.c` keeps its public-domain notice and does
not gain a new author line.

## Host checks

From this tree, with a host compiler whose `<linux/nl80211.h>` defines
`NL80211_ATTR_WIPHY_RADIOS`:

```sh
make host-checks
```

That configures `BUILD_DAEMON=OFF` and `BUILD_HOST_CHECKS=ON`, builds the
read-only planner and `test-core`, and runs `ctest`. The same planner check is
`./scripts/build.sh host-core`.

`linux-libc-dev` 6.8 does not define that attribute. Kernel 6.12 and 6.18 uapi
both give it the value 331. On such a host, pass a directory that contains
`linux/nl80211.h`:

```sh
make host-checks MESH11SD_NL80211_INCLUDE=/path/to/uapi
```

The overlay is a host compile aid. It is not shipped here and the OpenWrt
recipe does not use it.

`./scripts/build.sh host`, `sanitize`, `analyze`, and the `sdk-*` modes are the
parent-box suite. They use the existing OpenWrt 25.12.6 SDK trees under
`/workspace/openwrt-sdk`. This tree does not download or install an SDK.

Host checks need `cmake` on `PATH`. On the parent box that is the x86 SDK
host tool, not a separately installed package:

```sh
export PATH=/workspace/openwrt-sdk/openwrt-sdk-25.12.6-x86-64_gcc-14.3.0_musl.Linux-x86_64/staging_dir/host/bin:$PATH
```

The full host suite also needs the pinned dnsmasq 2.93 syntax oracle, which is
not shipped. Place it at `build/references/dnsmasq-2.93.tar.xz`.
`scripts/build-dnsmasq-checker.sh` accepts only SHA256
`0c00d4e5c97c8306e5fb932b348b34269c9c29a0e7df0e8e82958b407092bc19`.
The native half of that suite links libuci and libubus built from the same
x86 SDK's `dl/` archives. Direct `sdk-*` compiles use the matching target SDK.

## OpenWrt package

`openwrt/mesh11sd/Makefile` follows the apmond pattern: CMake package, apk,
`PKG_SOURCE`, and `PKG_HASH:=skip` until a real tarball exists. `PKG_VERSION`
is `0.0.0_pre8` and `PKG_RELEASE` is `0`. The package is architecture-specific
(`PKGARCH` is left unset). Declared dependencies are the base libraries
`libuci`, `libubus`, `libubox`, and `libblobmsg-json`.

The intended image is OpenWrt 25.12.6 with full `wpad-mbedtls` and that
image's default platform services, including netifd, dnsmasq, and fw4. Those
stay image contents. This package does not depend on extra packages for them.

Installed names:

- `/usr/sbin/mesh11sd-v8` from `peer_main.c`
- `/usr/sbin/mesh11sd-v8-plan` from `native_main.c`
- `/etc/init.d/mesh11sd-v8-experimental`
- `/etc/config/mesh11sd_v8_runtime` with `enabled` 0

The init script returns success without enabling or starting when apk or an
image build calls it. There is no package `postinst`. The daemon links with
`-static-libgcc`. Parent-box delivery names stay
`mesh11sd-v8-experimental-0.0.0_pre8-r0.apk` inside the SDK architecture
directory and `mesh11sd-v8-experimental-0.0.0_pre8-r0_${ARCH}.apk` for the
local delivered copy. Checksums of the three APKs already built are in
`openwrt/mesh11sd/VALIDATION.md`. Those APKs came from the pre-layout scratch
recipe. This CMake recipe has not rebuilt them.

`scripts/build-package.py` is the parent-box-only driver. It copies the recipe
into a private SDK workspace and writes
`dl/mesh11sd-v8-experimental-0.0.0_pre8.tar.gz` there. The archive's first
directory is `mesh11sd-v8-experimental-0.0.0_pre8`, which is the directory
OpenWrt's unpack expects. The make invocation does not set `USE_SOURCE_DIR`.
That variable is global on 25.12.6 and would point dependency builds such as
json-c and lua at this tree. The private workspace is the only SDK copy
written. The shared SDK and its feeds are not modified. No SDK build of this
corrected driver has been run here.

## What the running code is

`mesh11sd-v8` is an explicit foreground program. With no arguments it prints
usage and returns 2. The modes are `--run-openwrt-auto-ipv4`,
`--run-openwrt-peer`, and `--run-openwrt-ipv4-mrp`. Each requires root and an
OpenWrt root. `--version` and `--status` do not apply configuration.

Loop protection is inactive. The owned bridge is created with STP off, and
there is no nftables loop rule. A first hardware trial needs a topology
without a redundant non-mesh link.

Status reports hardware validation, Internet validation, loop protection,
bridged-client portal pinning, and IPv6 RA support as false. Own-WAN DNS
qualification is a source-bound configured-DNS check. It does not establish
general Internet reachability.

The read-only planner (`mesh11sd-plan --fixture` on the host, and
`mesh11sd-v8-plan` on the target) does not apply configuration. `--version`
and a status query also stay available when wpad is missing.

A mutating OpenWrt startup (`--run-openwrt-auto-ipv4`, `--run-openwrt-peer`,
or `--run-openwrt-ipv4-mrp`) waits until the apk database names
`wpad-mbedtls`, `wpad-openssl`, or `wpad-wolfssl` and that package's file list
matches the `/usr/sbin/wpad` multicall links. Unsuffixed `wpad` is the internal
build and is rejected: OpenWrt 25.12.6 does not enable `CONFIG_MESH` or
`CONFIG_OWE` for it. basic, mini, and mesh-only packages are rejected. The process logs that at most once every 60 seconds and
does not open the bus or create network state. `SIGHUP` rechecks immediately.
`SIGTERM` and `SIGINT` leave the wait. A privately built full wpad installed
under another package name is not recognized. `ubus get_features` is not used.

## Outstanding before any hardware beta

- Flashed-firmware compatibility on the two GL-MT3600BE boards named as the
  first hardware pair
- Bridged-client multi-portal gateway ARP policy
- Dynamic peer promotion, explicitly deferred. A running peer does not promote when WAN appears, and reentry is not implemented
- IPv6 RA, GUA, MBP, and ACS
- Loop protection, which stays inactive until a separate implementation exists

The parent box r2 retest passed 864 checks in each of host, sanitizer, and
analyzer, with 3 dnsmasq 2.93 parser cases and the private ubusd lifecycle in
each mode, plus 16 init fixtures, the CMake host checks, a direct filogic SDK
build, and a fresh filogic CMake APK. ath79 and mt76x8 were not rebuilt.
`tests/test_artifact_naming.py` covers its synthetic cases without those
outputs and inspects only a present filogic APK. Details are in
`docs/SCRATCH-MILESTONE.md` and `docs/STATUS.md`.
