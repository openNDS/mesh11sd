# Local experimental APK packaging checkpoint

The APK checksums and payload notes below are from the parent box, built with
the pre-layout scratch recipe (`beta/packaging/Makefile` direct `TARGET_CC`
compile) before this repository layout existed. This tree's
`openwrt/mesh11sd/Makefile` is a CMake recipe and has not been used to rebuild
those APKs. The recorded native filename
`mesh11sd-v8-experimental-0.0.0_pre8-r0.apk` and the delivered
`mesh11sd-v8-experimental-0.0.0_pre8-r0_${ARCH}.apk` names are unchanged.
`scripts/build-package.py` copies this recipe into a private SDK tree and
writes `dl/mesh11sd-v8-experimental-0.0.0_pre8.tar.gz` with the top directory
`mesh11sd-v8-experimental-0.0.0_pre8`. It does not pass `USE_SOURCE_DIR`.
The parent-box build of the previous driver failed because that variable also
redirected json-c and lua. This corrected driver has not been SDK-built here.

Commands in this tree:

```
python3 scripts/build-package.py filogic --stage-only
python3 scripts/verify-package.py filogic
./scripts/build.sh host
python3 tests/test_init.py
python3 tests/test_artifact_naming.py
```

`verify-package.py` and `--stage-only` need the retained APK and private SDK
workspace for the target being checked. `tests/test_artifact_naming.py` runs
its synthetic checks without any target APK. It inspects a filogic APK only
when `build/artifacts/filogic` is present, and it does not read ath79 or mt76x8.

# Original checkpoint record

Command paths in this original record are the scratch-tree paths
(`packaging/build-package.py`, `./build.sh`). They describe that checkpoint.
The layout paths are in the section above.

These are **actual matching OpenWrt25.12.6 SDK-built APKs**, not release artifacts
or a claim of a working/hardware-tested mesh. No APK was installed anywhere.
Fresh installation is disabled (`enabled '0'`); no boot links are in the payload.
The init script also refuses SDK install/upgrade auto-enable/start, including
when an old runtime configuration was explicitly enabled. This does not undo a
previous deliberate user opt-in. No v7 service/configuration was touched.

## Artifacts

Canonical apmond-style delivered copies under `build/artifacts/<target>/`:

- filogic: `mesh11sd-v8-experimental-0.0.0_pre8-r0_aarch64_cortex-a53.apk`
- ath79: `mesh11sd-v8-experimental-0.0.0_pre8-r0_mips_24kc.apk`
- mt76x8: `mesh11sd-v8-experimental-0.0.0_pre8-r0_mipsel_24kc.apk`

Native SDK filename `mesh11sd-v8-experimental-0.0.0_pre8-r0.apk` is retained beside
each delivered copy. Both are byte-identical, with the unchanged SHA256 below.
Actual APK metadata supplies all identity fields; architecture is also checked
against the real `bin/packages/<architecture>/base/` SDK output directory.
Internal metadata/archives are unchanged. No rebuilding was needed for naming.

Convention verified: official25.12.6 APK feeds and SDK `include/package-pack.mk`
lines306–308 use `name-version.apk` within an architecture directory, not an
architecture suffix. Older24.10 IPK feeds use `name_version_arch.ipk`. The agreed
apmond delivery convention preserves the native APK basename and adds `_ARCH`
before `.apk`. This is a local asset convention, not a claim about current APK
feed filenames. Official examples: `build/naming-convention/verified-convention.json`.

`build.json`/`verified.json` use `artifact` for the delivered path and retain
`sdk_artifact`, both filenames, and actual/SDK-output architecture. Normal future
builds stage both names automatically; to restage existing APKs without make:

```
python3 packaging/build-package.py filogic --stage-only
python3 packaging/build-package.py ath79 --stage-only
python3 packaging/build-package.py mt76x8 --stage-only
python3 tests/test_artifact_naming.py
```

Staging publishes a private temporary copy atomically without clobbering. Repeats
reuse identical bytes; changed native checksums, conflicting files/symlinks and
architecture disagreement fail explicitly. A future changed build with the same
package identity must explicitly resolve an existing different delivery file.
`build/artifacts/SHA256SUMS` lists only the three canonical delivered copies.

 The version is a local experimental packaging
identifier, not a release/tag. These packages are unsigned local SDK builds;
strict `apk verify` rejects `UNTRUSTED signature`. No signing keys were copied,
read or created. Do not treat these as authenticated distribution packages.

| SDK target | APK architecture | SHA256 |
|---|---|---|
| filogic | aarch64_cortex-a53 | `3d8f3f207fc2f44d53cdb9a4e20e61bc7cfc8d55282d11b14ea8c8b4f944bd15` |
| ath79 | mips_24kc | `b114b7a686054593ab1d539a6a59df3fcb121a9239a964116ddb00c764a42c4e` |
| mt76x8 | mipsel_24kc | `6b2f89d6f2220a35556c02b12f675d3cde8158b560d1bee752d9e6845f62c358` |

SDK paths are recorded exactly in each `build.json`/`verified.json`. Builds use
separate private SDK workspaces under `build/package-sdk-<target>`, not the shared
SDK/feed trees. No repository metadata or source-root signing keys are copied.
SDK-generated `origin: feeds/base/mesh11sd-v8-experimental` is a default metadata
label: this scratch package has **not** been added/published to any feed/repo.

Declared base runtime dependencies: `libc`, `libuci20250120`, `libubus20251202`,
`libubox20260213`, `libblobmsg-json20260213`. Target ELF dependencies match these
ABIs (planner does not require ubus). Existing netifd/wpad/dnsmasq/fw4 remain
platform prerequisites, not newly bundled runtime components. Compiler helpers
are statically linked using `-static-libgcc`, matching the existing direct SDK
checks. The first real payload inspection caught an unnecessary direct
`libgcc_s.so.1` dependency from the initial recipe; the final APKs remove it.

## Reproduce on this box

```
python3 packaging/build-package.py filogic
python3 packaging/verify-package.py filogic
python3 packaging/build-package.py ath79
python3 packaging/verify-package.py ath79
python3 packaging/build-package.py mt76x8
python3 packaging/verify-package.py mt76x8
./build.sh host
./build.sh sanitize
./build.sh analyze
./build.sh sdk-filogic
./build.sh sdk-ath79
./build.sh sdk-mt76x8
python3 tests/test_init.py
```

`verify-package.py` uses the private SDK apk-tools reader, **not installation**.
It checks metadata-derived filenames and byte equality with the native SDK APK,
then actual metadata/architecture/dependencies, exactly seven payload
files, root:root metadata, config0600, binary/init0755, default disabled config,
byte-for-byte init/config agreement, conffiles, generated script syntax and
pkgname-export ordering, exact target ELF class/endianness/musl loader/sonames,
and absence of RPATH/RUNPATH. It extracts to a temporary private directory with
`--no-chown`; it never executes target binaries or package install scripts.
It verifies integrity with `--allow-untrusted` **only on our own local artifact**
and proves a corrupted copy is rejected, separately from strict signature
rejection. Detailed evidence is in each artifact directory. The16 existing init
fixtures exercise the exact byte-identical packaged init, not real procd.

## Authenticated host prerequisites

Debian13/trixie amd64 was verified. GNU Make4.4.1 was already available. Normal
HTTPS curl to `deb.debian.org` downloaded official metadata/packages without
APT, proxy, DNS, source-list or network configuration changes. `sqv` verified
InRelease with the installed distro archive keyring and at least two valid
signatures; three valid public signer fingerprints are recorded. The signed
Release SHA256/size authenticated Packages.xz, then every .deb SHA256/size and
package/version/architecture was checked before dependency-ordered installation.
No installed packages were upgraded or removed.

Reusable cache/provenance: `/workspace/mesh11sd-host-prereqs/`, including signed
InRelease, verified Release, Packages.xz,15 verified .debs, dependency plan,
installer hashes/versions and `fetch.py`. Installed missing host packages:
libstdc++-14-dev, g++-14-x86-64-linux-gnu, g++-14, g++-x86-64-linux-gnu, g++,
libncurses6, libncurses-dev, libpopt0, rsync, libsigsegv2, gawk, bzip2,
libmagic-mgc, libmagic1t64, file. Flex/bison were not needed by these actual builds
and were not installed. `dpkg --audit` is clean. The existing Docker policy-rc.d
blocked rsync daemon activation; host package service files were installed, but
no host service was started. No mesh service was installed.

## Boundaries unchanged

The runtime C source, tests and corrective6a fixes are unchanged from milestone8.
The packaging checkpoint changed package linking and added a host verifier.
The subsequent naming-only checkpoint adds staging, filename validation/tests
and documentation; runtime code/init/config remain unchanged. All864 C checks passed again in each host/sanitizer/analyzer run, together with
3 real pinned dnsmasq2.93 syntax cases per mode,16 init fixtures and3 direct
matching-SDK builds. All three actual SDK APK builds and payload verifications
also passed, including negative corrupted-archive cases.
No target executable was run, no hardware/network apply was performed, and these
APKs have not been tested with an actual package manager/procd on a router.

Outstanding before usable hardware beta: controlled revised-hardware validation;
bridged-client multi-portal gateway ARP policy; dynamic peer promotion/reentry;
unsupported IPv6 RA/GUA/MBP/ACS and inactive loop management. Own-WAN DNS/route
qualification is not arbitrary Internet reachability proof. Pure IPv4 peer/MRP
code exists, but these package checks do not prove wireless joining, client
connectivity, route/NAT/DHCP correctness or failover on hardware.
