#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the complete pinned NVK Windows ARM64 ICD in an isolated overlay.

Windows nvkmd uses SDK-compiled KMT calls for memory, VA, queue and sync.
Physical device enumeration requires the live KMD's native-submit flag.
Requires the same Meson/Rust/bindgen LLVM-MinGW environment as the NAK tool,
plus build-native mesa_clc and vtn_bindgen2 in PATH (-Dmesa-clc=system).
No registry, device, driver install or external Mesa checkout is modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

REV = "0ff52079ca9e9e837ed27eb95a8d239f439a1864"
HERE = Path(__file__).resolve().parent


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mesa", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--cross-file", type=Path, required=True)
    p.add_argument("--cc", type=Path, required=True, help="ARM64 LLVM-MinGW clang")
    p.add_argument("--clang-cl", type=Path, required=True)
    p.add_argument("--sdk-include", type=Path, required=True, help="SDK Include/<version>")
    p.add_argument("--wdk-include", type=Path, required=True, help="WDK Include/<version>")
    p.add_argument("--prepare-only", action="store_true")
    a = p.parse_args()
    rev = subprocess.check_output(["git", "-C", str(a.mesa), "rev-parse", "HEAD"], text=True).strip()
    if rev != REV:
        p.error(f"Mesa HEAD must be {REV}")
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    archive = out / "mesa.tar"
    with archive.open("wb") as f:
        subprocess.run(["git", "-C", str(a.mesa), "archive", REV], stdout=f, check=True)
    src = out / "mesa-src"
    src.mkdir()
    with tarfile.open(archive) as t:
        t.extractall(src, filter="data")
    archive.unlink()
    for patch in ("mesa-windows-nak.patch", "mesa-windows-icd.patch"):
        run("git", "-C", src, "apply", HERE / patch)
    win = src / "src/nouveau/vulkan/nvkmd/windows"
    shutil.copytree(HERE / "nvkmd_windows", win)
    for abi in ("adapter_abi.h", "allocation_abi.h", "submit_abi.h"):
        shutil.copyfile(HERE.parent / "windows" / abi, win / abi)
    if a.prepare_only:
        print(f"Prepared {src}; not configured or built")
        return
    sdk_cc = (a.clang_cl, "--target=aarch64-pc-windows-msvc", "/c", "/std:c11",
        "/W4", "/WX", "-Wno-nonportable-include-path", "/D_ARM64_",
        "/imsvc" + str(a.sdk_include / "um"), "/imsvc" + str(a.sdk_include / "shared"),
        "/imsvc" + str(a.wdk_include / "km/crt"))
    run(*sdk_cc, "/Fo" + str(out / "kmt-sdk.obj"), "--", win / "test-kmt-sdk.c")
    run(*sdk_cc, "/O2", "/GS-", "/Zl", "/Fo" + str(win / "kmt-native.obj"),
        "--", win / "kmt_native.c")
    build = out / "build"
    # Changes to the overlay/transport must change the compiler cache identity.
    digest = hashlib.sha256(REV.encode())
    for name in ("mesa-windows-nak.patch", "mesa-windows-icd.patch"):
        digest.update((HERE / name).read_bytes())
    for f in sorted(win.glob("*")):
        digest.update(f.name.encode())
        digest.update(f.read_bytes())
    digest.update(a.cross_file.read_bytes())
    run("meson", "setup", build, src, "--cross-file", a.cross_file.resolve(),
        "-Dplatforms=windows", "-Dvulkan-drivers=nouveau", "-Dgallium-drivers=[]",
        "-Dglx=disabled", "-Degl=disabled", "-Dgbm=disabled", "-Dllvm=disabled",
        "-Dbuild-tests=false", "-Dvideo-codecs=[]", "-Dvalgrind=disabled",
        "-Dlibunwind=disabled", "-Dgles1=disabled", "-Dgles2=disabled", "-Dopengl=false",
        "-Dshader-cache=disabled", "-Dzlib=disabled", "-Dzstd=disabled", "-Dexpat=disabled",
        "-Dmesa-clc=system", "-Dbuildtype=release", "-Dnvk-build-id=" + digest.hexdigest())
    run("ninja", "-C", build, "src/nouveau/vulkan/libvulkan_nouveau.dll")
    for source, name in (("check.c", "nvk-windows-check.exe"), ("test-kmt.c", "test-kmt.exe")):
        run(a.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(src / "include"),
            "-I" + str(win), win / source, win / "kmt.c", "-o", out / name)
    dll = out / "libvulkan_nouveau.dll"
    shutil.copyfile(build / "src/nouveau/vulkan/libvulkan_nouveau.dll", dll)
    manifest = out / "nvk_windows_icd.json"
    manifest.write_text(json.dumps({"file_format_version": "1.0.0", "ICD": {
        "library_path": "./libvulkan_nouveau.dll", "api_version": "1.4.0",
    }}, indent=2) + "\n")
    files = [dll, out / "nvk-windows-check.exe", out / "test-kmt.exe", manifest]
    (out / "manifest.json").write_text(json.dumps({
        "mesa": REV, "overlay_id": digest.hexdigest(), "sdk_kmt_layout_verified": True,
        "scope": "NVK ICD with Windows KMT transport gated by the native-submit KMD flag",
        "files": {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
