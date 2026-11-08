#!/usr/bin/env python3
"""Copy the signed GPU test kit without macOS metadata or sidecar files."""
import argparse
import hashlib
from pathlib import Path
import shutil

FILES = (
    "switchfan/switchfan.inf", "switchfan/switchfan.sys", "switchfan/switchfan.cat",
    "tegragpuprobe/tegragpuprobe.inf", "tegragpuprobe/tegragpuprobe.sys",
    "tegragpuprobe/tegragpuprobe.cat", "LitePerformanceCheck.exe",
    "Install-GpuTest.ps1", "Install-GpuTest.cmd", "GpuTest-CodeSigning.cer",
)


def stage(kit, destination):
    # An explicit payload list excludes Finder files and resource forks.
    payload = {name: (kit / name).read_bytes() for name in FILES}
    if kit.resolve() == destination.resolve():
        raise ValueError("Choose a destination different from the source kit")
    destination.mkdir(parents=True, exist_ok=True)
    for name, content in payload.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        # write_bytes copies no xattrs, ACLs, resource forks or Finder metadata.
        target.write_bytes(content)
        if hashlib.sha256(target.read_bytes()).digest() != hashlib.sha256(content).digest():
            raise OSError(f"Copy readback mismatch: {name}")
    # Clean only metadata in this test kit, never other SD folders or volumes.
    for item in sorted(destination.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        if item.name in (".DS_Store", "__MACOSX") or item.name.startswith("._"):
            if item.is_dir() and not item.is_symlink():
                shutil.rmtree(item)
            else:
                item.unlink()
    print(f"Copied and read back {len(FILES)} payload files: {destination}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kit", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    args = parser.parse_args()
    stage(args.kit, args.destination)
