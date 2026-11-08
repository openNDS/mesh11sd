#!/bin/sh
# Host-only real parser oracle, never installed or started as a DNS/DHCP service.
# Required input, not shipped in the source tree:
#   build/references/dnsmasq-2.93.tar.xz
#   SHA256 0c00d4e5c97c8306e5fb932b348b34269c9c29a0e7df0e8e82958b407092bc19
# scripts/build.sh host|sanitize|analyze calls this after the C checks.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
umask 077
vendor="$root/build/vendor/dnsmasq-syntax"
archive="$root/build/references/dnsmasq-2.93.tar.xz"
mkdir -p "$vendor"
exec 9>"$vendor/.build.lock"
flock 9
printf '%s  %s\n' '0c00d4e5c97c8306e5fb932b348b34269c9c29a0e7df0e8e82958b407092bc19' "$archive" | sha256sum -c - > "$root/build/dnsmasq-syntax-source.log"
if [ ! -x "$vendor/dnsmasq-2.93/src/dnsmasq" ]; then
    tar -xJf "$archive" -C "$vendor"
    make -C "$vendor/dnsmasq-2.93" -j2 VERSION=2.93 \
        COPTS='-DNO_IDN -DNO_LUA -DNO_IPSET -DNO_NFTSET' > "$root/build/dnsmasq-syntax-build.log" 2>&1
fi
"$vendor/dnsmasq-2.93/src/dnsmasq" --version > "$root/build/dnsmasq-syntax-version.log"
printf '%s\n' "$vendor/dnsmasq-2.93/src/dnsmasq"
