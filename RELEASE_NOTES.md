# mesh11sd 0.0.0_pre8-r0 — unpublished experimental notes

There is no release. `0.0.0_pre8-r0` is the packaging identifier already used
for the parent-box APKs. It is not a Git tag and it is not 8.0.0beta.

## This working tree

The C sources from the scratch handoff are arranged like apmond 2.0.1beta's
repository shape: `src/lib`, `src/mesh11sd`, root `CMakeLists.txt`, `tests`,
`openwrt/mesh11sd`, `.build_commit`, and `.gitattributes` `export-subst`.
The daemon, the UCI model, and the package name are mesh11sd's. apmond's
collector, crypto, and monitoring code are not imported.

`.build_commit` still contains the export-subst token `$Format:%h$`. CMake
reports `build_commit=uncommitted` while that token is present. It does not
read `git rev-parse HEAD`, because HEAD on this checkout is the historical
7.3.2beta commit.

## Package behavior carried into the recipe

- OpenWrt 25.12.6 apk, one architecture per build, on an image that already
  has full `wpad-mbedtls` and the default platform services
- Dependencies: `libuci`, `libubus`, `libubox`, `libblobmsg-json` only
- Binaries `/usr/sbin/mesh11sd-v8` and `/usr/sbin/mesh11sd-v8-plan`
- Config `/etc/config/mesh11sd_v8_runtime`, mode 0600, `enabled` 0
- Init refuses enable/start during install and image build
- No `postinst`, no ucode program, no `pkill`
- `-static-libgcc` so the package does not depend on `libgcc_s`
- `PKG_HASH:=skip` until an authorized tarball exists

## Still outstanding

Loop protection is inactive. Bridged-client multi-portal ARP, IPv6 RA/GUA/MBP,
and ACS are unimplemented. Dynamic peer promotion is explicitly deferred.
The first hardware pair is two GL-MT3600BE boards, and flashed firmware
compatibility is still to be verified. The three parent-box APKs listed in
`openwrt/mesh11sd/VALIDATION.md` were built from the scratch recipe before
this layout and have not been rebuilt or installed.

## Historical 7.x

7.x release history stays in `ChangeLog`. The shell implementation and its
manual stay on branch `7.3.2beta`.
