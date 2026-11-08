#!/bin/sh
# Parent-box and host builds. SDK trees and feeds are never modified.
# host-core is the portable planner check. host/sanitize/analyze/sdk-* match
# the pre-layout scratch suite and need the parent box's SDK tree.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
mode=${1:-host-core}
case "$mode" in
    host-core)
        cmake -S "$root" -B "$root/build/cmake-host" -DBUILD_DAEMON=OFF -DBUILD_HOST_CHECKS=ON
        cmake --build "$root/build/cmake-host"
        ctest --test-dir "$root/build/cmake-host" --output-on-failure
        printf 'PASS: host-core (%s)\n' "$root/build/cmake-host"
        exit 0
        ;;
    host|sanitize|analyze)
        compiler=${CC:-cc}
        output=build/$mode
        ;;
    sdk-filogic|sdk-ath79|sdk-mt76x8)
        compiler=${CC:-cc}
        output=build/$mode
        ;;
    *)
        printf '%s\n' 'usage: build.sh host-core|host|sanitize|analyze|sdk-filogic|sdk-ath79|sdk-mt76x8' >&2
        exit 2
        ;;
esac
case "$mode" in
    sdk-filogic)
        sdk=/workspace/openwrt-sdk/openwrt-sdk-25.12.6-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64
        toolchain=toolchain-aarch64_cortex-a53_gcc-14.3.0_musl
        triple=aarch64-openwrt-linux-musl
        ;;
    sdk-ath79)
        sdk=/workspace/openwrt-sdk/openwrt-sdk-25.12.6-ath79-generic_gcc-14.3.0_musl.Linux-x86_64
        toolchain=toolchain-mips_24kc_gcc-14.3.0_musl
        triple=mips-openwrt-linux-musl
        ;;
    sdk-mt76x8)
        sdk=/workspace/openwrt-sdk/openwrt-sdk-25.12.6-ramips-mt76x8_gcc-14.3.0_musl.Linux-x86_64
        toolchain=toolchain-mipsel_24kc_gcc-14.3.0_musl
        triple=mipsel-openwrt-linux-musl
        ;;
esac
case "$mode" in
    sdk-*)
        if [ ! -x "$sdk/staging_dir/$toolchain/bin/$triple-gcc" ]; then
            printf '%s\n' 'matching OpenWrt 25.12.6 SDK is not on this machine; SDK builds stay on the parent box' >&2
            exit 1
        fi
        compiler=$sdk/staging_dir/$toolchain/bin/$triple-gcc
        flags='-Os -static-libgcc'
        STAGING_DIR=$sdk/staging_dir
        export STAGING_DIR
        ;;
    host)
        flags='-O2 -g'
        ;;
    analyze)
        flags='-O0 -g -fanalyzer'
        ;;
    sanitize)
        flags='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'
        ;;
esac
lib=$root/src/lib
daemon=$root/src/mesh11sd
nl80211=
if [ -n "${MESH11SD_NL80211_INCLUDE:-}" ]; then
    nl80211="-isystem ${MESH11SD_NL80211_INCLUDE}"
fi
mkdir -p "$output"
"$compiler" --version > "$output/compiler.txt"
# flags and nl80211 are fixed local compiler switches, never user data.
# shellcheck disable=SC2086
"$compiler" $flags -std=c11 -Wall -Wextra -Werror -Wpedantic -D_DEFAULT_SOURCE -I"$lib" $nl80211 \
    "$daemon/main.c" "$lib/factory.c" "$lib/identity.c" "$lib/plan.c" "$lib/portal.c" \
    "$lib/platform.c" "$lib/sha256.c" -o "$output/mesh11sd-plan"
# shellcheck disable=SC2086
"$compiler" $flags -std=c11 -Wall -Wextra -Werror -Wpedantic -D_DEFAULT_SOURCE -I"$lib" $nl80211 \
    "$root/tests/test_core.c" "$lib/factory.c" "$lib/identity.c" "$lib/plan.c" "$lib/portal.c" \
    "$lib/platform.c" "$lib/sha256.c" -o "$output/test-core"
case "$mode" in
    host|sanitize|analyze)
        "$output/test-core" > "$output/tests.log"
        "$output/mesh11sd-plan" --fixture > "$output/fixture.log"
        python3 "$root/tests/test_cli.py" "$output/mesh11sd-plan" > "$output/cli-smoke.log"
        # shellcheck disable=SC2086
        "$compiler" $flags -std=gnu11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -I"$lib" \
            "$root/tests/test_wpad_guard.c" "$lib/wpad_guard.c" "$lib/safeio.c" "$lib/sha256.c" \
            -o "$output/test-wpad-guard"
        "$output/test-wpad-guard" > "$output/wpad-guard-tests.log"
        ;;
    sdk-*)
        printf 'SDK=%s\nCC=%s\n' "$sdk" "$compiler" > "$output/sdk.txt"
        readelf -h -l -d "$output/mesh11sd-plan" > "$output/elf-verification.log"
        ;;
esac
"$root/scripts/build-native.sh" "$mode" "$compiler" "$output" "${sdk:-}"
printf 'PASS: %s build (%s)\n' "$mode" "$output"
