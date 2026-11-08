#!/usr/bin/env python3
"""Real pinned ubusd over a private Unix socket only; no system service/network."""
from pathlib import Path
import subprocess
import json
import os
import pwd
import signal
import sys
import tempfile
import time
with tempfile.TemporaryDirectory(prefix="mesh-v8-status-") as root:
    socket = Path(root) / "ubus.sock"
    acl = Path(root) / "acl"
    acl.mkdir(mode=0o700)
    acl_file = acl / "public-test-only.json"
    acl_file.write_text(json.dumps({
        "user": pwd.getpwuid(os.getuid()).pw_name,
        "publish": ["mesh11sd.v8", "PUBLIC-TEST-ONLY.unrelated"],
        "access": {"mesh11sd.v8": {"methods": ["status"]}, "PUBLIC-TEST-ONLY.unrelated": {"methods": ["ping"]}}
    }))
    with open(Path(root) / "ubusd.log", "w+") as log:
        # The actual pinned ubusd requires root-owned ACL metadata. Only this
        # one synthetic file gets chowned; broker/client stay unprivileged.
        # SDK fakeroot cannot intercept new-glibc stat() here, so do not fake it.
        acl_file.chmod(0o644)
        subprocess.run(["sudo", "-n", "chown", "0:0", str(acl_file)], check=True, timeout=5)
        daemon = subprocess.Popen([sys.argv[2], "-s", str(socket), "-A", str(acl)], stdout=log, stderr=log, start_new_session=True)
        try:
            deadline = time.monotonic() + 5
            while not socket.exists() and daemon.poll() is None and time.monotonic() < deadline:
                time.sleep(0.01)
            if not socket.exists() or daemon.poll() is not None:
                log.seek(0)
                raise AssertionError("private ubusd startup failed: " + log.read())
            subprocess.run([sys.argv[1], str(socket)], check=True, timeout=30)
        finally:
            os.killpg(daemon.pid, signal.SIGTERM)
            daemon.wait(timeout=5)
print("PASS: actual pinned ubusd private-AF_UNIX status lifecycle; system bus untouched")
