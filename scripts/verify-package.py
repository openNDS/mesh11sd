#!/usr/bin/env python3
"""Inspect our local SDK APK, never install it or execute target code/scripts.

Signature rejection is recorded, not hidden: these private experimental SDK
artifacts are not signed/trusted distribution packages. --allow-untrusted is
used only for local payload integrity checking/extraction, never installation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from artifact_staging import artifact_names

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("target", choices=("filogic", "ath79", "mt76x8"))
args = parser.parse_args()
os.umask(0o077)
root = Path(__file__).resolve().parent.parent
out = root / "build/artifacts" / args.target
record = json.loads((out / "build.json").read_text())
archive = root / record["artifact"]
assert archive.parent.resolve() == out.resolve()
assert hashlib.sha256(archive.read_bytes()).hexdigest() == record["sha256"]
tool = Path(record["private_workspace"]) / "staging_dir/host/bin/apk"

def apk(*arguments, check=True):
    return subprocess.run([str(tool), "--no-network", *map(str, arguments)],
                          capture_output=True, text=True, timeout=30, check=check)

metadata = json.loads(apk("adbdump", "--format", "json", archive).stdout)
(out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
info = metadata["info"]
sdk_filename, delivery_filename = artifact_names(info)
assert archive.name == delivery_filename == record["delivery_filename"]
assert record["architecture"] == record["sdk_output_architecture"] == info["arch"]
native = root / record["sdk_artifact"]
assert native.parent.resolve() == out.resolve() and native.name == sdk_filename
assert not native.is_symlink() and not archive.is_symlink()
assert hashlib.sha256(native.read_bytes()).hexdigest() == record["sha256"]
assert native.read_bytes() == archive.read_bytes(), "delivery copy must not alter the native APK"
architectures = {"filogic": "aarch64_cortex-a53", "ath79": "mips_24kc",
                 "mt76x8": "mipsel_24kc"}
assert info["name"] == "mesh11sd-v8-experimental"
assert info["version"] == "0.0.0_pre8-r0"
assert info["arch"] == architectures[args.target]
assert set(info["depends"]) == {"libc", "libuci20250120", "libubus20251202",
                               "libubox20260213", "libblobmsg-json20260213"}
expected = {"etc/config/mesh11sd_v8_runtime": 0o600,
            "etc/init.d/mesh11sd-v8-experimental": 0o755,
            "usr/sbin/mesh11sd-v8": 0o755,
            "usr/sbin/mesh11sd-v8-plan": 0o755}
for suffix in ("conffiles", "conffiles_static", "list"):
    expected["lib/apk/packages/mesh11sd-v8-experimental." + suffix] = 0o644
files = {}
for directory in metadata["paths"]:
    acl = directory["acl"]
    assert acl == {"mode": 0o755, "user": "root", "group": "root"}
    name = directory.get("name", "")
    assert not name.startswith("/") and ".." not in Path(name).parts
    for entry in directory.get("files", []):
        filename = str(Path(name) / entry["name"])
        assert filename in expected and filename not in files
        assert entry["acl"] == {"mode": expected[filename], "user": "root", "group": "root"}
        assert "target" not in entry, "no payload symlinks expected"
        files[filename] = entry
assert set(files) == set(expected), "unexpected/missing payload or boot links"
strict = apk("verify", archive, check=False)
assert strict.returncode != 0 and "UNTRUSTED signature" in strict.stderr + strict.stdout
integrity = apk("--allow-untrusted", "verify", archive)
(out / "signature-verification.log").write_text("exit_code=" + str(strict.returncode) + "\n" + strict.stdout + strict.stderr)
(out / "integrity-verification.log").write_text("exit_code=0\n" + integrity.stdout + integrity.stderr)
elf = {}
with tempfile.TemporaryDirectory(prefix="inspect-only-", dir=out) as temporary:
    extract_root = Path(temporary)
    assert extract_root.resolve() != root.resolve()
    payload = extract_root / "payload"
    payload.mkdir()
    apk("--allow-untrusted", "extract", "--no-chown", "--destination", payload, archive)
    actual = {str(path.relative_to(payload)) for path in payload.rglob("*") if path.is_file()}
    assert actual == set(expected)
    assert not any(path.is_symlink() for path in payload.rglob("*"))
    for name, entry in files.items():
        path = payload / name
        assert path.stat().st_size == entry["size"]
        assert path.stat().st_mode & 0o777 == expected[name]
        assert hashlib.sha256(path.read_bytes()).hexdigest() == entry["hash"]
    for package_path, source_name in (("etc/config/mesh11sd_v8_runtime", "mesh11sd_v8_runtime.config"),
                                      ("etc/init.d/mesh11sd-v8-experimental", "mesh11sd-v8.init")):
        repository_file = root / "openwrt/mesh11sd/files" / source_name
        assert repository_file.is_file(), "repository package file missing"
        assert not repository_file.resolve().is_relative_to(extract_root.resolve())
        assert (payload / package_path).read_bytes() == repository_file.read_bytes()
    config = (payload / "etc/config/mesh11sd_v8_runtime").read_text()
    assert re.search(r"option\s+enabled\s+['\"]?0['\"]?(?:\s|$)", config)
    assert (payload / "lib/apk/packages/mesh11sd-v8-experimental.conffiles").read_text().strip() == "/etc/config/mesh11sd_v8_runtime"
    # Read target ELF headers/dynamic sections; never execute target binaries.
    for name in ("mesh11sd-v8", "mesh11sd-v8-plan"):
        binary = payload / "usr/sbin" / name
        data = binary.read_bytes()
        assert data[:4] == b"\x7fELF"
        expected_machine, expected_class, expected_endian = {"filogic": (183, 2, 1), "ath79": (8, 1, 2), "mt76x8": (8, 1, 1)}[args.target]
        assert data[4] == expected_class and data[5] == expected_endian
        assert int.from_bytes(data[18:20], "little" if data[5] == 1 else "big") == expected_machine
        details = subprocess.run(["readelf", "-h", "-l", "-d", str(binary)], capture_output=True, text=True, timeout=10, check=True).stdout
        (out / (name + ".readelf.txt")).write_text(details)
        needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", details)
        required = {"libc.so", "libuci.so.20250120", "libubus.so.20251202",
                    "libubox.so.20260213", "libblobmsg_json.so.20260213"}
        if name == "mesh11sd-v8-plan":
            required.remove("libubus.so.20251202")
        assert set(needed) == required
        assert not re.search(r"\((?:RPATH|RUNPATH)\)", details)
        interpreter = re.search(r"Requesting program interpreter: ([^\]]+)", details).group(1)
        interpreters = {"filogic": "/lib/ld-musl-aarch64.so.1",
                        "ath79": "/lib/ld-musl-mips-sf.so.1",
                        "mt76x8": "/lib/ld-musl-mipsel-sf.so.1"}
        assert interpreter == interpreters[args.target]
        elf[name] = {"machine": expected_machine, "class": expected_class,
                     "endianness": expected_endian, "interpreter": interpreter, "needed": needed}
    # Corrupt an isolated copy; the integrity checker must really reject it.
    corrupt = extract_root / "corrupt.apk"
    data = bytearray(archive.read_bytes())
    data[len(data) // 2] ^= 0x80
    corrupt.write_bytes(data)
    rejected = apk("--allow-untrusted", "verify", corrupt, check=False)
    assert rejected.returncode != 0, "payload corruption was not rejected"
    (out / "corruption-rejection.log").write_text("exit_code=" + str(rejected.returncode) + "\n" + rejected.stdout + rejected.stderr)
    # Parse generated scripts only; do not execute their real default_postinst.
    assert set(metadata["scripts"]) == {"post-install", "post-upgrade", "pre-deinstall"}
    for name, text in metadata["scripts"].items():
        script = extract_root / name
        script.write_text(text)
        subprocess.run(["/bin/sh", "-n", str(script)], check=True, timeout=5)
        assert 'export pkgname="mesh11sd-v8-experimental"' in text
        hook = "default_prerm" if name == "pre-deinstall" else "default_postinst"
        assert hook in text
        if name != "pre-deinstall":
            assert text.index('export pkgname=') < text.index('default_postinst')
        assert "postinst-pkg" not in text and "uci commit" not in text
result = dict(record, inspection={"metadata_architecture": info["arch"], "depends": info["depends"],
              "payload_files": sorted(files), "metadata_ownership": "root:root", "config_mode": "0600",
              "enabled_default": False, "boot_links": False, "integrity_verified": True,
              "corruption_rejected": True, "signature_trusted": False,
              "signature_note": "Local SDK mkpkg without --sign-key; strict verify rejects UNTRUSTED signature",
              "elf": elf, "generated_scripts": "syntax and pkgname ordering verified; not installed/executed"})
(out / "verified.json").write_text(json.dumps(result, indent=2) + "\n")
print(args.target + ": verified7 payload files, architecture/base dependencies, root ownership, disabled default, scripts, ELF and corruption rejection; signature NOT trusted; NOT installed")
