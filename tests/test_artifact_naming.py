#!/usr/bin/env python3
"""Naming checks on synthetic fixture bytes, plus filogic when that APK is present.

The synthetic checks do not read ath79 or mt76x8 outputs and do not claim those
targets were built. A present filogic artifact set is inspected with its own
apk tool. A missing filogic set is reported and is not counted as a pass.
"""
import hashlib
import json
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

repository = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(repository / "scripts"))
from artifact_staging import artifact_names, stage_delivery

checks = 0

def check(name, condition):
    global checks
    checks += 1
    assert condition, name
    print("ok", checks, "-", name)


def rejects(name, function, exception=ValueError):
    try:
        function()
    except exception:
        check(name, True)
    else:
        check(name, False)


def fixture_bytes(arch):
    return b"SYNTHETIC-APK " + arch.encode() + b"\nPUBLIC-TEST-ONLY\n"


def write_fake_apk(path):
    path.write_text("""#!/usr/bin/env python3
import json
import sys
from pathlib import Path
line = Path(sys.argv[-1]).read_bytes().splitlines()[0]
arch = line.split()[1].decode()
json.dump({"info": {"name": "mesh11sd-v8-experimental", "version": "0.0.0_pre8-r0", "arch": arch}}, sys.stdout)
""")
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


native_name = "mesh11sd-v8-experimental-0.0.0_pre8-r0.apk"
for arch in ("synthetic_a", "synthetic_b", "synthetic_c"):
    info = {"name": "mesh11sd-v8-experimental", "version": "0.0.0_pre8-r0", "arch": arch}
    sdk_name, delivered_name = artifact_names(info)
    check("synthetic metadata controls native and delivered names: " + arch,
          sdk_name == native_name and delivered_name == native_name[:-4] + "_" + arch + ".apk")

with tempfile.TemporaryDirectory(prefix="artifact-naming-synthetic-") as temporary:
    work = Path(temporary)
    tool = work / "apk"
    write_fake_apk(tool)
    for arch in ("synthetic_a", "synthetic_b", "synthetic_c"):
        output = work / arch
        output.mkdir()
        native = output / native_name
        native.write_bytes(fixture_bytes(arch))
        digest = hashlib.sha256(native.read_bytes()).hexdigest()
        stage_delivery(native, output, tool, arch, digest)
        delivered = output / (native_name[:-4] + "_" + arch + ".apk")
        stamp = (delivered.stat().st_ino, delivered.stat().st_mtime_ns)
        result = stage_delivery(native, output, tool, arch, digest)
        check("synthetic repeat staging keeps inode/mtime and exact bytes: " + arch,
              result["sha256"] == digest and native.read_bytes() == delivered.read_bytes() and
              stamp == (delivered.stat().st_ino, delivered.stat().st_mtime_ns))

valid = {"name": "mesh11sd-v8-experimental", "version": "0.0.0_pre8-r0", "arch": "synthetic_a"}
for field, value in (("name", "../escape"), ("version", "bad/filename"), ("arch", "bad\\name"),
                     ("arch", "bad\nname"), ("arch", None), ("name", "évil"),
                     ("name", "x" * 161), ("arch", "")):
    malformed = dict(valid)
    malformed[field] = value
    rejects("unsafe/missing/oversized metadata rejected: " + field, lambda: artifact_names(malformed))

with tempfile.TemporaryDirectory(prefix="artifact-naming-refusal-") as temporary:
    work = Path(temporary)
    tool = work / "apk"
    write_fake_apk(tool)
    native = work / native_name
    native.write_bytes(fixture_bytes("synthetic_a"))
    output = work / "delivery"
    output.mkdir()
    original = hashlib.sha256(native.read_bytes()).hexdigest()
    rejects("SDK architecture disagreement rejected", lambda: stage_delivery(native, output, tool, "wrong_arch", original))
    rejects("changed checksum rejected", lambda: stage_delivery(native, output, tool, "synthetic_a", "0" * 64))
    check("failed validation publishes nothing", not list(output.iterdir()))
    delivery = output / artifact_names(valid)[1]
    delivery.write_bytes(b"PUBLIC-TEST-ONLY-CONFLICT")
    rejects("different delivery never overwritten", lambda: stage_delivery(native, output, tool, "synthetic_a", original), FileExistsError)
    check("conflict preserves native and existing delivery", hashlib.sha256(native.read_bytes()).hexdigest() == original and delivery.read_bytes() == b"PUBLIC-TEST-ONLY-CONFLICT")
    delivery.unlink()
    delivery.symlink_to(native)
    rejects("delivery symlink rejected", lambda: stage_delivery(native, output, tool, "synthetic_a", original), FileExistsError)
    source_link = work / "source-link.apk"
    source_link.symlink_to(native)
    rejects("native source symlink rejected", lambda: stage_delivery(source_link, output, tool, "synthetic_a", original))

assert checks == 21, "synthetic naming coverage changed: " + str(checks)
print(str(checks) + " synthetic naming checks; no other target artifact was read")

filogic = repository / "build/artifacts/filogic"
record_path = filogic / "build.json"
if not record_path.is_file():
    print("filogic real APK integration not run: build/artifacts/filogic/build.json is absent")
    raise SystemExit(0)
record = json.loads(record_path.read_text())
native = repository / record["sdk_artifact"]
delivered = repository / record["artifact"]
tool = Path(record["private_workspace"]) / "staging_dir/host/bin/apk"
if not native.is_file() or not delivered.is_file() or not tool.is_file():
    print("filogic real APK integration not run: native APK, delivered APK, or apk tool is absent")
    raise SystemExit(0)
metadata_path = filogic / "metadata.json"
if metadata_path.is_file():
    info = json.loads(metadata_path.read_text())["info"]
else:
    dumped = subprocess.run([str(tool), "--no-network", "adbdump", "--format", "json", str(native)],
                            check=True, capture_output=True, text=True, timeout=30)
    info = json.loads(dumped.stdout)["info"]
sdk_name, delivered_name = artifact_names(info)
check("filogic real APK metadata controls native and delivered names",
      info["arch"] == "aarch64_cortex-a53" and sdk_name == native_name and
      delivered_name == native_name[:-4] + "_aarch64_cortex-a53.apk" and
      native.name == sdk_name and delivered.name == delivered_name)
stamp = (delivered.stat().st_ino, delivered.stat().st_mtime_ns)
result = stage_delivery(native, filogic, tool, record["sdk_output_architecture"], record["sha256"])
check("filogic real APK repeat staging keeps inode/mtime and exact native bytes",
      result["sha256"] == record["sha256"] and native.read_bytes() == delivered.read_bytes() and
      stamp == (delivered.stat().st_ino, delivered.stat().st_mtime_ns))
print("filogic real APK integration: 2 checks. No other target was read or claimed.")
