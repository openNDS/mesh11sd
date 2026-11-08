"""Local delivery names derived from APK metadata; archives are never edited.

OpenWrt25 APK feeds use name-version.apk within an architecture directory.
Older OpenWrt IPKs use name_version_arch.ipk. The agreed apmond-style delivered
copy is name-version_arch.apk. This explicit local asset convention retains the
native APK basename and adds an underscore plus the actual metadata architecture;
it is not claimed to be the native25 APK feed filename convention.
"""
import hashlib
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def artifact_names(info):
    patterns = {"name": r"[a-z0-9][a-z0-9+.-]*",
                "version": r"[A-Za-z0-9][A-Za-z0-9._+~:-]*",
                "arch": r"[a-z0-9][a-z0-9_-]*"}
    for key, pattern in patterns.items():
        value = info.get(key)
        if not isinstance(value, str) or len(value) > 160 or not re.fullmatch(pattern, value):
            raise ValueError("unsupported APK identity field: " + key)
    native = info["name"] + "-" + info["version"] + ".apk"
    delivery = native[:-4] + "_" + info["arch"] + ".apk"
    if max(len(native), len(delivery)) > 255:
        raise ValueError("APK delivery filename exceeds filesystem limit")
    return native, delivery


def write_checksum_manifests(output, filename, digest):
    def atomic_text(path, text):
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", prefix=".sums-", dir=path.parent, delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(text)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    # Independent SDK builds can finish concurrently; serialize only manifests.
    with (output.parent / ".artifact-sums.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        atomic_text(output / "SHA256SUMS", digest + "  " + filename + "\n")
        lines = []
        for manifest in sorted(output.parent.glob("*/SHA256SUMS")):
            for line in manifest.read_text().splitlines():
                checksum, name = line.split("  ", 1)
                if not re.fullmatch(r"[0-9a-f]{64}", checksum) or Path(name).name != name:
                    raise ValueError("malformed local delivery checksum manifest")
                file = manifest.parent / name
                if file.is_symlink() or hashlib.sha256(file.read_bytes()).hexdigest() != checksum:
                    raise ValueError("delivery manifest no longer matches retained bytes")
                lines.append(checksum + "  " + manifest.parent.name + "/" + name + "\n")
        atomic_text(output.parent / "SHA256SUMS", "".join(lines))


def stage_delivery(native, output, apk_tool, sdk_architecture, expected_sha=None):
    native = Path(native)
    output = Path(output)
    if native.is_symlink() or not native.is_file() or not output.is_dir():
        raise ValueError("regular native APK and existing output directory required")
    digest = hashlib.sha256(native.read_bytes()).hexdigest()
    if expected_sha is not None and digest != expected_sha:
        raise ValueError("retained native APK checksum changed")
    response = subprocess.run([str(apk_tool), "--no-network", "adbdump", "--format", "json", str(native)],
                              check=True, capture_output=True, text=True, timeout=30)
    info = json.loads(response.stdout)["info"]
    sdk_name, delivered_name = artifact_names(info)
    if info["arch"] != sdk_architecture:
        raise ValueError("APK metadata disagrees with SDK output directory architecture")
    if native.name != sdk_name:
        raise ValueError("native SDK filename disagrees with actual APK metadata")
    delivered = output / delivered_name
    if delivered.exists() or delivered.is_symlink():
        if delivered.is_symlink() or not delivered.is_file() or hashlib.sha256(delivered.read_bytes()).hexdigest() != digest:
            raise FileExistsError("refusing to overwrite a different delivery artifact")
    else:
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(prefix=".delivery-", dir=output, delete=False) as stream:
                temporary = Path(stream.name)
                with native.open("rb") as source:
                    shutil.copyfileobj(source, stream)
                stream.flush()
                os.fsync(stream.fileno())
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != digest:
                raise ValueError("native APK changed while staging")
            # Atomic publication without clobbering an existing file/symlink.
            os.link(temporary, delivered)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    if hashlib.sha256(native.read_bytes()).hexdigest() != digest:
        raise ValueError("native APK changed during staging")
    write_checksum_manifests(output, delivered_name, digest)
    return {"architecture": info["arch"], "sdk_output_architecture": sdk_architecture, "sdk_filename": sdk_name,
            "delivery_filename": delivered_name, "sha256": digest,
            "filename_convention": "apmond-style local name-version_arch.apk; native APK name-version.apk retained"}
