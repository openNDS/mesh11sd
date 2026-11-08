#!/usr/bin/env python3
"""Isolated public synthetic fixtures; no configuration survives execution."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="mesh11sd-native-test-") as root:
    result = subprocess.run([sys.argv[1], root], timeout=30)
    if result.returncode:
        sys.exit(result.returncode)
    native = str(Path(sys.argv[1]).resolve().parent / "mesh11sd-native")
    config = Path(root) / "etc/config/mesh11sd"
    original = config.read_bytes()
    check = subprocess.run([native, "--config-check", root], capture_output=True, timeout=10)
    assert check.returncode == 0, "native committed config check failed"
    assert config.read_bytes() == original, "read-only CLI changed committed input"
    assert b"TEST-ONLY" not in check.stdout + check.stderr
    assert b"--__" not in check.stdout + check.stderr
    # Explicitly public noncredential marker checks error diagnostic redaction.
    marker = b"TEST-ONLY-MALFORMED-KEY-DO-NOT-LOG"
    config.write_bytes(b"config mesh11sd 'setup'\n option auto_mesh_key '" + marker + b"\n")
    check = subprocess.run([native, "--config-check", root], capture_output=True, timeout=10)
    assert check.returncode != 0, "malformed UCI accepted"
    assert marker not in check.stdout + check.stderr, "parser echoed sensitive input"
    config.write_bytes(original)
    check = subprocess.run([native, "--apply"], capture_output=True, timeout=10)
    assert check.returncode == 2, "unvalidated live apply entrypoint exposed"
    print("PASS: native CLI committed-input, nonmutation, redaction, malformed-input and apply-gate smoke")
