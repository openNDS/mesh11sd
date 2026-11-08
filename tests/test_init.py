#!/usr/bin/env python3
"""PUBLIC-TEST-ONLY: shell lifecycle request fixture; never system procd/config."""
from pathlib import Path
import os
import subprocess
import tempfile

script = Path(__file__).resolve().parent.parent / "openwrt/mesh11sd/files/mesh11sd-v8.init"
harness = r'''
config_load() { [ "$1" = mesh11sd_v8_runtime ]; }
config_get() {
    case "$1:$3" in
        enabled:enabled) enabled="$PUBLIC_TEST_ENABLED" ;;
        mode:mode) mode="$PUBLIC_TEST_MODE" ;;
        *) return 2 ;;
    esac
}
procd_open_instance() { printf 'open:%s\n' "$*"; }
procd_set_param() { printf 'param:%s\n' "$*"; }
procd_close_instance() { printf 'close\n'; }
procd_send_signal() { printf 'signal:%s\n' "$*"; }
stop() { printf 'stop\n'; }
ln() { printf 'ln:%s\n' "$*"; }
. "$1"
"$2"
'''
checks = 0
def run(action, *, enabled="1", mode="auto-ipv4", package="", image=""):
    env = dict(os.environ, PUBLIC_TEST_ENABLED=enabled, PUBLIC_TEST_MODE=mode, pkgname=package, IPKG_INSTROOT=image)
    return subprocess.run(["/bin/sh", "-c", harness, "PUBLIC-TEST-ONLY", str(script), action], env=env, capture_output=True, text=True, timeout=5)
def check(name, condition):
    global checks
    checks += 1
    assert condition, name
    print("ok", checks, "-", name)
for action in ("enable", "start_service", "reload_service"):
    result = run(action, package="mesh11sd-v8-experimental")
    check("APK install/upgrade never starts, enables or reloads even if old config enabled: " + action, result.returncode == 0 and result.stdout == "" and result.stderr == "")
for action in ("enable", "start_service"):
    result = run(action, image="PUBLIC-TEST-ONLY-image-root")
    check("image assembly never starts or enables beta: " + action, result.returncode == 0 and result.stdout == "")
result = run("start_service", enabled="0")
check("disabled explicit start constructs no instance", result.returncode == 0 and result.stdout == "")
result = run("enable", enabled="0")
check("disabled explicit enable cannot create boot symlinks", result.returncode != 0 and result.stdout == "")
for mode, argument in (("auto-ipv4", "--run-openwrt-auto-ipv4"), ("peer", "--run-openwrt-peer"), ("ipv4-mrp", "--run-openwrt-ipv4-mrp")):
    result = run("start_service", mode=mode)
    expected = "open:main\nparam:command /usr/sbin/mesh11sd-v8 " + argument + "\nparam:stdout 0\nparam:stderr 1\nparam:term_timeout 120\nclose\n"
    check("one exact native procd command, no respawn or secret argv: " + mode, result.returncode == 0 and result.stdout == expected and result.stderr == "")
with tempfile.TemporaryDirectory(prefix="mesh-v8-init-test-") as root:
    marker = Path(root) / "should-not-exist"
    result = run("start_service", mode="auto-ipv4; touch " + str(marker))
    check("untrusted runtime mode cannot become shell code or process arguments", result.returncode != 0 and result.stdout == "" and not marker.exists())
result = run("reload_service")
check("reload is one owned HUP, not another procd instance", result.returncode == 0 and result.stdout == "signal:mesh11sd-v8-experimental main 1\n")
result = run("reload_service", enabled="0")
check("disabled reload requests only stop", result.returncode == 0 and result.stdout == "stop\n")
result = run("enable")
check("explicit opted-in enable targets only two private service links", result.returncode == 0 and result.stdout == "ln:-sf ../init.d/mesh11sd-v8-experimental /etc/rc.d/S95mesh11sd-v8-experimental\nln:-sf ../init.d/mesh11sd-v8-experimental /etc/rc.d/K10mesh11sd-v8-experimental\n")
package_pack = Path("/workspace/openwrt-sdk/openwrt-sdk-25.12.6-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64/include/package-pack.mk").read_text()
check("install guards match the real SDK APK pkgname export/default_postinst ordering", "export pkgname=" in package_pack and package_pack.index('echo "default_postinst"') < package_pack.index("post-install;"))
check("service does not enable shell tracing or a speculative respawn", "set +x" in script.read_text() and "procd_set_param respawn" not in script.read_text())
print(str(checks) + " init fixture checks; real procd/network not executed")
