#!/usr/bin/env python3
"""Run packet-path tests and compile an ARM64 Windows COFF object, not a driver."""
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
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
args.out.mkdir(parents=True, exist_ok=True)
source = root / "core/msgbuf_data.c"
common = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror"]
test = args.out / "test-msgbuf-data"
subprocess.run(common + ["-g", "-O1", "-fsanitize=address,undefined",
    "-fno-sanitize-recover=all", str(source), str(root / "tests/test_data.c"),
    "-o", str(test)], check=True)
subprocess.run([str(test)], check=True)
obj = args.out / "msgbuf_data.obj"
subprocess.run(common + ["--target=aarch64-pc-windows-msvc", "-ffreestanding",
    "-fno-builtin", "-fms-extensions", "-O2", "-D_ARM64_", "-D_WIN64",
    "-isystem", str(args.wdk_include / "km/crt"),
    "-isystem", str((args.sdk_include or args.wdk_include) / "shared"),
    "-c", str(source), "-o", str(obj)], check=True)
if obj.read_bytes()[:2] != b"\x64\xaa":
    sys.exit("The cross-compiled object is not ARM64 COFF")
print(f"ARM64 packet core: {obj}")
print("This object is not an installable Wi-Fi adapter or a hardware test result.")
