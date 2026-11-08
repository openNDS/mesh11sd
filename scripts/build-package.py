#!/usr/bin/env python3
"""Box-only local SDK package build; never writes the shared SDK/feed tree.

The OpenWrt recipe is openwrt/mesh11sd/Makefile. This driver copies that recipe
into a private SDK workspace and places one source archive in that workspace's
dl/ directory. OpenWrt unpacks it only for this package. Dependencies keep the
SDK's own dl/ archives. USE_SOURCE_DIR is not set: on 25.12.6 it is global and
makes json-c, lua, and other prepares symlink at this tree.

No SDK is installed here. A machine without the matching 25.12.6 SDK cannot
cross-compile.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
from artifact_staging import stage_delivery

parser = argparse.ArgumentParser()
parser.add_argument("target", choices=("filogic", "ath79", "mt76x8"))
mode = parser.add_mutually_exclusive_group()
mode.add_argument("--prepare-only", action="store_true")
mode.add_argument("--stage-only", action="store_true", help="name-only staging of the retained actual APK; no make or SDK writes")
args = parser.parse_args()
os.umask(0o077)
root = Path(__file__).resolve().parent.parent
sdk_suffix = {"filogic": "mediatek-filogic", "ath79": "ath79-generic", "mt76x8": "ramips-mt76x8"}[args.target]
source = Path("/workspace/openwrt-sdk") / ("openwrt-sdk-25.12.6-" + sdk_suffix + "_gcc-14.3.0_musl.Linux-x86_64")
assert source.is_dir() and (source / "include/package-pack.mk").is_file()
work = root / "build" / ("package-sdk-" + args.target)
lock = os.open(str(root / "build" / ("package-" + args.target + ".lock")), os.O_RDWR | os.O_CREAT, 0o600)
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
marker = work / ".mesh11sd-private-sdk"
identity = str(source) + "\n"
if args.stage_only:
    assert marker.is_file() and marker.read_text() == identity, "private matching SDK required"
    out = root / "build/artifacts" / args.target
    record = json.loads((out / "build.json").read_text())
    native = root / record.get("sdk_artifact", record["artifact"])
    assert native.parent.resolve() == out.resolve()
    originals = sorted((work / "bin/packages").glob("*/*/" + native.name))
    assert len(originals) == 1, "exactly one retained SDK output required"
    original = originals[0]
    assert hashlib.sha256(original.read_bytes()).hexdigest() == record["sha256"]
    named = stage_delivery(native, out, work / "staging_dir/host/bin/apk",
                           original.parent.parent.name, record["sha256"])
    record.update(named, sdk_artifact=str(native.relative_to(root)),
                  artifact=str((out / named["delivery_filename"]).relative_to(root)))
    (out / "build.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, sort_keys=True), flush=True)
    raise SystemExit(0)
if not work.exists():
    print("Preparing isolated matching SDK workspace", args.target, flush=True)
    def ignore(directory, names):
        omitted = {name for name in names if name in (".git", ".hg", ".svn")}
        # Never copy/read another build's signing keys or generated artifacts.
        if Path(directory) == source:
            omitted.update(name for name in names if name.startswith("key-build") or name in ("bin", "tmp", "logs"))
        return omitted
    shutil.copytree(source, work, symlinks=True, ignore=ignore)
    marker.write_text(identity)
else:
    assert marker.is_file() and marker.read_text() == identity, "not our private matching SDK workspace"
assert not work.is_symlink() and work.parent.resolve() == (root / "build").resolve()
# copytree preserves the SDK root's mode; restore the private enclosing mode.
work.chmod(0o700)
if args.prepare_only:
    print("Private SDK prepared; no shared SDK/feed changes", flush=True)
    raise SystemExit(0)
# Drop a leftover global source override from the broken driver. It is not used.
leftover = work / "source-tree"
if leftover.exists() or leftover.is_symlink():
    if leftover.is_symlink() or leftover.is_file():
        leftover.unlink()
    else:
        shutil.rmtree(leftover)
source_name = "mesh11sd-v8-experimental-0.0.0_pre8.tar.gz"
source_top = "mesh11sd-v8-experimental-0.0.0_pre8"
dl = work / "dl"
dl.mkdir(mode=0o755, exist_ok=True)
archive = dl / source_name
if archive.exists() or archive.is_symlink():
    archive.unlink()
# GNU tar stores the working-tree modes. The first component is the OpenWrt
# build-directory name, which is what include/unpack.mk extracts into BUILD_DIR.
# Archive the checkout's entries, not ".", so the archive has no "." member.
entries = sorted(path.name for path in root.iterdir()
                 if path.name not in {".git", "build"} and not path.name.endswith(".apk"))
subprocess.run(["tar", "--format=posix", "--sort=name", "-czf", str(archive),
                "--exclude=.git", "--exclude=__pycache__", "--exclude=*.pyc",
                "--transform", "s,^," + source_top + "/,",
                "-C", str(root), *entries], check=True)
with tarfile.open(archive, "r:gz") as packed:
    names = packed.getnames()
assert names, "package source archive is empty"
assert all(name == source_top or name.startswith(source_top + "/") for name in names), "archive root must be the package build directory"
assert source_top + "/CMakeLists.txt" in names
assert source_top + "/src/mesh11sd/peer_main.c" in names
assert source_top + "/.build_commit" in names
assert not any(name == source_top + "/.git" or name.startswith(source_top + "/.git/") or
               "/__pycache__/" in name or name.endswith(".pyc") or name.endswith(".apk")
               for name in names)
assert not any(name == source_top + "/build" or name.startswith(source_top + "/build/") for name in names)
package = work / "package" / "mesh11sd-v8-experimental"
if package.exists():
    shutil.rmtree(package)
package.mkdir(parents=True)
shutil.copy2(root / "openwrt/mesh11sd/Makefile", package / "Makefile")
shutil.copytree(root / "openwrt/mesh11sd/files", package / "files")
config = work / ".config"
text = config.read_text()
text = re.sub(r'^# CONFIG_PACKAGE_mesh11sd-v8-experimental is not set\n|^CONFIG_PACKAGE_mesh11sd-v8-experimental=.*\n', '', text, flags=re.M)
config.write_text(text + "\nCONFIG_PACKAGE_mesh11sd-v8-experimental=m\n")
env = dict(os.environ)
env.pop("STAGING_DIR", None)
env.pop("USE_SOURCE_DIR", None)
# Leave the original host PATH for SDK prerequisite discovery. Prepending the
# SDK host/bin here makes SetupHostCommand create self-referential tool links.
for key in ("STAGING_DIR_HOST", "STAGING_DIR_HOSTPKG", "ORIG_PATH"):
    env.pop(key, None)
# SDK requires umask022 for package payload metadata. Workspace itself is0700;
# private ownership journals/backups are independent0600, never packaged.
os.umask(0o022)
subprocess.run(["make", "--version"], cwd=work, env=env, check=True)
subprocess.run(["make", "defconfig", "V=s"], cwd=work, env=env, check=True)
subprocess.run(["make", "package/mesh11sd-v8-experimental/compile", "V=s", "-j2"],
               cwd=work, env=env, check=True)
artifacts = sorted((work / "bin").rglob("mesh11sd-v8-experimental*.apk"))
assert len(artifacts) == 1, "exactly one actual SDK-built experimental APK required"
out = root / "build/artifacts" / args.target
out.mkdir(parents=True, exist_ok=True)
artifact = out / artifacts[0].name
shutil.copy2(artifacts[0], artifact)
assert artifacts[0].parent.parent.parent == work / "bin/packages"
named = stage_delivery(artifact, out, work / "staging_dir/host/bin/apk", artifacts[0].parent.parent.name)
metadata = {"sdk": str(source), "private_workspace": str(work), "target": args.target,
            "sdk_artifact": str(artifact.relative_to(root)),
            "artifact": str((out / named["delivery_filename"]).relative_to(root)), **named,
            "hardware_validated": False, "public_release": False, "installed": False,
            "recipe": "openwrt/mesh11sd/Makefile", "layout_sdk_rebuild": True}
(out / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(json.dumps(metadata, sort_keys=True), flush=True)
