#!/bin/sh
# Invoked by build.sh. Host libuci/libubus objects stay under build/vendor.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
mode=$1
compiler=$2
output=$3
sdk=$4
lib=$root/src/lib
daemon=$root/src/mesh11sd
host_sdk=/workspace/openwrt-sdk/openwrt-sdk-25.12.6-x86-64_gcc-14.3.0_musl.Linux-x86_64
host=$host_sdk/staging_dir/host
sources=$(sed "s|^|$lib/|" "$root/scripts/native-sources.txt" | tr '\n' ' ')
peer_sources=$(sed "s|^|$lib/|" "$root/scripts/peer-sources.txt" | tr '\n' ' ')
case "$mode" in
    host|sanitize|analyze)
        if [ ! -d "$host_sdk" ]; then
            printf '%s\n' 'host OpenWrt SDK is not on this machine; native checks stay on the parent box' >&2
            exit 1
        fi
        vendor="$root/build/vendor"
        source_dir=$vendor/uci-2025.12.02~66127cd7
        parser=$vendor/uci-host
        mkdir -p "$vendor"
        if [ ! -f "$parser/libuci.a" ]; then
            if [ ! -d "$source_dir" ]; then
                PATH="$host/bin:$PATH" tar --zstd -xf "$host_sdk/dl/uci-2025.12.02~66127cd7.tar.zst" -C "$vendor"
            fi
            "$host/bin/cmake" -S "$source_dir" -B "$parser" -DBUILD_LUA=OFF -DBUILD_STATIC=ON \
                -Dubox_include_dir="$host/include" -Dubox="$host/lib/libubox.so" \
                -Dubox-static="$host/lib/libubox.a" > "$output/native-parser-configure.log"
            "$host/bin/cmake" --build "$parser" --target uci-static --parallel 2 > "$output/native-parser-build.log"
        fi
        ubus_source=$vendor/ubus-2026.06.28~24864e78
        ubus_build=$vendor/ubus-host
        if [ ! -f "$ubus_build/libubus.a" ]; then
            if [ ! -d "$ubus_source" ]; then
                PATH="$host/bin:$PATH" tar --zstd -xf "$host_sdk/dl/ubus-2026.06.28~24864e78.tar.zst" -C "$vendor"
            fi
            "$host/bin/cmake" -S "$ubus_source" -B "$ubus_build" -DBUILD_LUA=OFF -DBUILD_EXAMPLES=OFF -DBUILD_STATIC=ON \
                -Dubox_include_dir="$host/include" -Dubox_library="$host/lib/libubox.a" \
                -Dblob_library="$host/lib/libblobmsg_json.a" -Djson="$host/lib/libjson-c.a" > "$output/ubus-configure.log"
            "$host/bin/cmake" --build "$ubus_build" --target ubus --parallel 2 > "$output/ubus-build.log"
        fi
        flags='-O2 -g'
        if [ "$mode" = sanitize ]; then
            flags='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'
        fi
        if [ "$mode" = analyze ]; then
            flags='-O0 -g -fanalyzer'
        fi
        # All word-split lists below are fixed local build switches and sources.
        # No configuration input or credential is passed to any shell command.
        for entry in "$daemon/native_main.c" "$root/tests/test_native.c"; do
            binary=mesh11sd-native
            if [ "$entry" = "$root/tests/test_native.c" ]; then
                binary=test-native
            fi
            # shellcheck disable=SC2086
            "$compiler" $flags -std=gnu11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -I"$lib" \
                -I"$source_dir" -isystem "$host/include" "$entry" $sources \
                "$parser/libuci.a" "$host/lib/libblobmsg_json.a" "$host/lib/libubox.a" \
                "$host/lib/libjson-c.a" -lm -o "$output/$binary"
        done
        python3 "$root/tests/run_native.py" "$output/test-native" > "$output/native-tests.log"
        for entry in "$daemon/peer_main.c" "$root/tests/test_peer.c" "$root/tests/test_services.c" "$root/tests/test_bridge.c" "$root/tests/test_mrp.c" "$root/tests/test_portal_runtime.c" "$root/tests/test_wan.c" "$root/tests/test_status.c"; do
            binary=mesh11sd-peer
            if [ "$entry" = "$root/tests/test_peer.c" ]; then
                binary=test-peer
            fi
            if [ "$entry" = "$root/tests/test_services.c" ]; then
                binary=test-services
            fi
            if [ "$entry" = "$root/tests/test_bridge.c" ]; then
                binary=test-bridge
            fi
            if [ "$entry" = "$root/tests/test_mrp.c" ]; then
                binary=test-mrp
            fi
            if [ "$entry" = "$root/tests/test_portal_runtime.c" ]; then
                binary=test-portal-runtime
            fi
            if [ "$entry" = "$root/tests/test_wan.c" ]; then
                binary=test-wan
            fi
            if [ "$entry" = "$root/tests/test_status.c" ]; then
                binary=test-status
            fi
            # shellcheck disable=SC2086
            "$compiler" $flags -std=gnu11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -I"$lib" -I"$root/tests" \
                -I"$source_dir" -I"$ubus_source" -isystem "$host/include" "$entry" $sources $peer_sources \
                "$parser/libuci.a" "$ubus_build/libubus.a" "$host/lib/libblobmsg_json.a" \
                "$host/lib/libubox.a" "$host/lib/libjson-c.a" -lm -o "$output/$binary"
        done
        python3 "$root/tests/run_peer.py" "$output/test-peer" > "$output/peer-tests.log"
        python3 "$root/tests/run_peer.py" "$output/test-services" > "$output/services-tests.log"
        python3 "$root/tests/run_peer.py" "$output/test-bridge" > "$output/bridge-tests.log"
        python3 "$root/tests/run_peer.py" "$output/test-portal-runtime" > "$output/portal-runtime-tests.log"
        python3 "$root/tests/run_peer.py" "$output/test-wan" > "$output/wan-tests.log"
        "$host/bin/cmake" --build "$ubus_build" --target ubusd --parallel 2 > "$output/status-ubusd-build.log"
        "$root/scripts/build-ubusd-fixture.sh" "$compiler" "$output"
        python3 "$root/tests/run_status.py" "$output/test-status" "$output/ubusd-fixture" > "$output/status-tests.log"
        checker=$("$root/scripts/build-dnsmasq-checker.sh")
        python3 "$root/tests/run_peer.py" "$output/test-mrp" "$checker" > "$output/mrp-tests.log"
        ;;
    sdk-*)
        set -- "$sdk"/staging_dir/target-*/usr/include/uci.h
        [ "$#" -eq 1 ] && [ -f "$1" ]
        staging=${1%/usr/include/uci.h}
        # shellcheck disable=SC2086
        "$compiler" -Os -static-libgcc -std=gnu11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE \
            -I"$lib" -isystem "$staging/usr/include" "$daemon/native_main.c" $sources -L"$staging/usr/lib" \
            -Wl,-rpath-link,"$staging/usr/lib" -luci -lblobmsg_json -lubox -o "$output/mesh11sd-native"
        readelf -h -l -d "$output/mesh11sd-native" > "$output/native-elf-verification.log"
        # shellcheck disable=SC2086
        "$compiler" -Os -static-libgcc -std=gnu11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE \
            -I"$lib" -isystem "$staging/usr/include" "$daemon/peer_main.c" $sources $peer_sources -L"$staging/usr/lib" \
            -Wl,-rpath-link,"$staging/usr/lib" -luci -lblobmsg_json -lubus -lubox -lm -o "$output/mesh11sd-peer"
        readelf -h -l -d "$output/mesh11sd-peer" > "$output/peer-elf-verification.log"
        printf 'Native base libraries: libuci libblobmsg_json libubox; peer also libubus (OpenWrt base)\n' >> "$output/sdk.txt"
        ;;
esac
