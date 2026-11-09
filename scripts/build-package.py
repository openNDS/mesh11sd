#!/usr/bin/env python3
"""Box-only local SDK package build for mesh11sd v8-port. Never writes the shared SDK tree."""
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

parser = argparse.ArgumentParser()
parser.add_argument("target", choices=("filogic", "ath79", "mt76x8", "x86_64"))
parser.add_argument("--prepare-only", action="store_true")
args = parser.parse_args()
os.umask(0o077)

PKG_NAME = "mesh11sd"
PKG_VERSION = "8.0.0_alpha11"
PKG_RELEASE = "1"
source_top = f"{PKG_NAME}-{PKG_VERSION}"
source_name = f"{source_top}.tar.gz"

root = Path(__file__).resolve().parent.parent
sdk_suffix = {
    "filogic": "mediatek-filogic",
    "ath79": "ath79-generic",
    "mt76x8": "ramips-mt76x8",
    "x86_64": "x86-64",
}[args.target]
source = Path("/workspace/openwrt-sdk") / (
    f"openwrt-sdk-25.12.6-{sdk_suffix}_gcc-14.3.0_musl.Linux-x86_64"
)
assert source.is_dir() and (source / "include/package-pack.mk").is_file()

work = root / "build" / ("package-sdk-" + args.target)
work.parent.mkdir(parents=True, exist_ok=True)
lock = os.open(str(root / "build" / ("package-" + args.target + ".lock")),
               os.O_RDWR | os.O_CREAT, 0o600)
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
marker = work / ".mesh11sd-private-sdk"
identity = str(source) + "\n"

if not work.exists():
    print("Preparing isolated matching SDK workspace", args.target, flush=True)

    def ignore(directory, names):
        omitted = {n for n in names if n in (".git", ".hg", ".svn")}
        if Path(directory) == source:
            omitted.update(
                n for n in names
                if n.startswith("key-build") or n in ("bin", "tmp", "logs")
            )
        return omitted

    shutil.copytree(source, work, symlinks=True, ignore=ignore)
    marker.write_text(identity)
else:
    assert marker.is_file() and marker.read_text() == identity

assert not work.is_symlink() and work.parent.resolve() == (root / "build").resolve()
work.chmod(0o700)
if args.prepare_only:
    print("Private SDK prepared", flush=True)
    raise SystemExit(0)

leftover = work / "source-tree"
if leftover.exists() or leftover.is_symlink():
    if leftover.is_symlink() or leftover.is_file():
        leftover.unlink()
    else:
        shutil.rmtree(leftover)

dl = work / "dl"
dl.mkdir(mode=0o755, exist_ok=True)
archive = dl / source_name
if archive.exists() or archive.is_symlink():
    archive.unlink()

skip = {".git", "build", "mesh11sd"}
entries = sorted(
    path.name for path in root.iterdir()
    if path.name not in skip and not path.name.endswith(".apk")
    and not path.name.endswith(".o")
)
subprocess.run(
    [
        "tar", "--format=posix", "--sort=name", "-czf", str(archive),
        "--exclude=.git", "--exclude=__pycache__", "--exclude=*.pyc",
        "--exclude=*.o",
        "--transform", f"s,^,{source_top}/,",
        "-C", str(root), *entries,
    ],
    check=True,
)

with tarfile.open(archive, "r:gz") as packed:
    names = packed.getnames()
assert names
assert all(n == source_top or n.startswith(source_top + "/") for n in names)
assert source_top + "/CMakeLists.txt" in names
assert source_top + "/src/mesh11sd/main.c" in names
assert not any(n == source_top + "/.git" or n.startswith(source_top + "/.git/") for n in names)
assert not any(n.startswith(source_top + "/build/") or n == source_top + "/build" for n in names)

# Drop leftover experimental package dir if present
for stale in ("mesh11sd-v8-experimental",):
    stale_path = work / "package" / stale
    if stale_path.exists():
        shutil.rmtree(stale_path)

package = work / "package" / PKG_NAME
if package.exists():
    shutil.rmtree(package)
package.mkdir(parents=True)
shutil.copy2(root / "openwrt/mesh11sd/Makefile", package / "Makefile")
shutil.copytree(root / "openwrt/mesh11sd/files", package / "files")

config = work / ".config"
text = config.read_text()
text = re.sub(
    r"^# CONFIG_PACKAGE_mesh11sd is not set\n|^CONFIG_PACKAGE_mesh11sd=.*\n|"
    r"^# CONFIG_PACKAGE_mesh11sd-v8-experimental is not set\n|"
    r"^CONFIG_PACKAGE_mesh11sd-v8-experimental=.*\n",
    "",
    text,
    flags=re.M,
)
config.write_text(text + "\nCONFIG_PACKAGE_mesh11sd=m\n")

env = dict(os.environ)
for key in ("STAGING_DIR", "USE_SOURCE_DIR", "STAGING_DIR_HOST",
            "STAGING_DIR_HOSTPKG", "ORIG_PATH"):
    env.pop(key, None)

os.umask(0o022)
subprocess.run(["make", "defconfig", "V=s"], cwd=work, env=env, check=True)
subprocess.run(
    ["make", f"package/{PKG_NAME}/compile", "V=s", "-j2"],
    cwd=work, env=env, check=True,
)

artifacts = sorted((work / "bin").rglob(f"{PKG_NAME}*.apk"))
# Prefer the exact versioned apk, ignore unrelated
artifacts = [a for a in artifacts if PKG_VERSION in a.name]
assert len(artifacts) == 1, f"expected one apk, got {[a.name for a in artifacts]}"

out = root / "build" / "artifacts" / args.target
out.mkdir(parents=True, exist_ok=True)
artifact = out / artifacts[0].name
shutil.copy2(artifacts[0], artifact)
sha = hashlib.sha256(artifact.read_bytes()).hexdigest()
# delivery name as requested
delivery = f"{PKG_NAME}-{PKG_VERSION}-r{PKG_RELEASE}_aarch64_cortex-a53.apk"
# Only rename for filogic; other targets keep SDK name
if args.target == "filogic":
    delivery_path = out / delivery
    if delivery_path != artifact:
        shutil.copy2(artifact, delivery_path)
else:
    delivery_path = artifact
    delivery = artifact.name

meta = {
    "sdk": str(source),
    "target": args.target,
    "artifact": str(delivery_path),
    "sha256": hashlib.sha256(delivery_path.read_bytes()).hexdigest(),
    "pkg_version": PKG_VERSION,
    "pkg_release": PKG_RELEASE,
}
(out / "build.json").write_text(json.dumps(meta, indent=2) + "\n")
print(json.dumps(meta, sort_keys=True), flush=True)
