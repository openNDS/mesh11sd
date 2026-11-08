#!/usr/bin/env python3
"""Only public fixtures and injected RPC/kernel callbacks; never live ubus."""
from pathlib import Path
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix="mesh11sd-peer-test-") as root:
    result = subprocess.run([sys.argv[1], root], timeout=60)
    if result.returncode:
        sys.exit(result.returncode)
    if len(sys.argv) == 3:
        checker = str(Path(sys.argv[2]).resolve())
        version = subprocess.run([checker, "--version"], capture_output=True, timeout=5, check=True).stdout
        assert b"Dnsmasq version 2.93 " in version
        for name in ("syntax-mrp.conf", "syntax-suppression.conf"):
            config = Path(root) / name
            assert config.is_file(), name
            checked = subprocess.run([checker, "--test", "--port=0", "-C", str(config)], capture_output=True, timeout=5)
            assert checked.returncode == 0, (name, checked.stderr.decode())
            print("PASS: real pinned dnsmasq2.93 --test " + name)
        invalid = Path(root) / "syntax-known-invalid.conf"
        invalid.write_text("# PUBLIC-TEST-ONLY old two-set-tag regression\ndhcp-range=set:lan,set:mesh11sd_v8,192.0.2.100,192.0.2.200,255.255.255.0,3600s\n")
        checked = subprocess.run([checker, "--test", "--port=0", "-C", str(invalid)], capture_output=True, timeout=5)
        assert checked.returncode != 0 and b"only one tag allowed" in checked.stderr
        print("PASS: real pinned dnsmasq2.93 rejects former two-set-tag grammar")
    binary = str(Path(sys.argv[1]).resolve().parent / "mesh11sd-peer")
    result = subprocess.run([binary], capture_output=True, timeout=5)
    assert result.returncode == 2
    version = subprocess.run([binary, "--version"], capture_output=True, timeout=5)
    assert version.returncode == 0 and b"experimental C scratch" in version.stdout
    # Deliberately do NOT execute the enabled OpenWrt mode on the box.
    print("PASS: peer CLI defaults to no action; live mode not executed")
