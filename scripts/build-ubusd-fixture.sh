#!/bin/sh
# Exact pinned ubusd source/library. Override only its unused DEFAULT socket
# directory so mkdir_sockdir stays in scratch even when -s uses a private socket.
# No source patch; no system bus, host /run directory, or host network access.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
compiler=$1
output=$2
host=/workspace/openwrt-sdk/openwrt-sdk-25.12.6-x86-64_gcc-14.3.0_musl.Linux-x86_64/staging_dir/host
source_dir=$root/build/vendor/ubus-2026.06.28~24864e78
ubus_build=$root/build/vendor/ubus-host
"$compiler" -O2 -g -std=gnu11 -DUBUS_MAX_MSGLEN=1048576 \
    "-DUBUS_UNIX_SOCKET=\"$root/build/ubusd-fixture-default.sock\"" \
    -isystem "$host/include" "$source_dir/ubusd_main.c" "$ubus_build/libubusd_library.a" \
    -Wl,--start-group "$host/lib/libubox.a" "$host/lib/libblobmsg_json.a" "$host/lib/libjson-c.a" \
    -Wl,--end-group -o "$output/ubusd-fixture"
