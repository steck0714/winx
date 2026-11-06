#!/usr/bin/env python3
"""Test msgbuf and build an ARM64 KMDF transport library, not a driver package."""
import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", default="clang", help="LLVM clang executable")
parser.add_argument("--wdk-include", type=Path, required=True,
                    help="WDK Include/<version> containing km/crt and shared")
parser.add_argument("--sdk-include", type=Path,
                    help="SDK Include/<version> if extracted separately from the WDK")
parser.add_argument("--kmdf-include", type=Path,
                    help="KMDF headers; defaults to WDK Include/wdf/kmdf/1.33")
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
args.out.mkdir(parents=True, exist_ok=True)
sources = [root / "core/msgbuf_data.c", root / "core/msgbuf_ring.c", root / "core/msgbuf_control.c"]
common = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror"]
for name in ("data", "ring", "control", "erom", "nvram"):
    test = args.out / f"test-msgbuf-{name}"
    subprocess.run(common + ["-g", "-O1", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", *map(str, sources), str(root / f"tests/test_{name}.c"),
        "-o", str(test)], check=True)
    subprocess.run([str(test)], check=True)
kmdf = args.kmdf_include or args.wdk_include.parent / "wdf/kmdf/1.33"
kernel_cc = Path(args.cc).with_name("clang-cl" + Path(args.cc).suffix)
kernel_flags = [str(kernel_cc), "--target=aarch64-pc-windows-msvc", "/c",
    "/kernel", "/GS-", "/Zl", "/O2", "/W4", "/WX",
    "-Wno-unused-parameter", "-Wno-microsoft-anon-tag", "-Wno-ignored-pragma-intrinsic",
    "-Wno-pragma-pack", "-Wno-invalid-token-paste", "-Wno-unknown-pragmas",
    "-Wno-ignored-attributes", "-Wno-nonportable-include-path",
    "/D_ARM64_", "/D_WIN64", "/D_KERNEL_MODE",
    "/DKMDF_VERSION_MAJOR=1", "/DKMDF_VERSION_MINOR=33",
    "/DNTDDI_VERSION=0x0A00000C", "/D_WIN32_WINNT=0x0A00",
    f"/imsvc{args.wdk_include / 'km/crt'}", f"/imsvc{args.wdk_include / 'km'}",
    f"/imsvc{(args.sdk_include or args.wdk_include) / 'shared'}", f"/imsvc{kmdf}"]
objects = []
for source in sources + [root / "windows/transport.c", root / "windows/resources.c",
                        root / "windows/chip.c", root / "windows/shared.c",
                        root / "windows/control.c"]:
    obj = args.out / (source.stem + ".obj")
    # On macOS, an absolute /Users/... path otherwise looks like clang-cl /U.
    subprocess.run(kernel_flags + [f"/Fo{obj}", "--", str(source)], check=True)
    if obj.read_bytes()[:2] != b"\x64\xaa":
        sys.exit(f"The cross-compiled object is not ARM64 COFF: {obj}")
    objects.append(obj)
libtool = Path(args.cc).with_name("llvm-lib" + Path(args.cc).suffix)
library = args.out / "SwitchWifiTransport.lib"
subprocess.run([str(libtool), "/nologo", f"/out:{library}", *map(str, objects)], check=True)
print(f"ARM64 KMDF transport library: {library}")
print("This library is not an installable Wi-Fi adapter or a hardware test result.")
