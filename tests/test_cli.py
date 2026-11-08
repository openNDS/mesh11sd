#!/usr/bin/env python3
"""Read-only CLI smoke tests. Synthetic inputs only; never alter networking."""
import json
import subprocess
import sys

binary = sys.argv[1]
fixture = subprocess.run([binary, "--fixture"], text=True, capture_output=True, check=True, timeout=10)
lines = fixture.stdout.splitlines()
assert lines[0].startswith("SYNTHETIC TEST FIXTURE ONLY;")
summary = json.loads(lines[1])
assert summary["mode"] == "plan-only"
assert summary["applied"] is False
assert summary["loop_protection_active"] is False
assert "credential" not in fixture.stdout
assert "--__" not in fixture.stdout
assert "TEST-ONLY-NOT-A-DEPLOYMENT" not in fixture.stdout
for arguments, code in [
    (["--probe-bridge", "lo"], 1),
    (["--probe-radio", "4294967295", "2412"], 1),
    (["--probe-radio", "0", "-1"], 2),
    (["--apply"], 2),
    ([], 2),
]:
    result = subprocess.run([binary] + arguments, text=True, capture_output=True, timeout=10)
    assert result.returncode == code, (arguments, result.returncode)
print("PASS: synthetic fixture JSON/redaction and five read-only/refused-operation CLI cases")
