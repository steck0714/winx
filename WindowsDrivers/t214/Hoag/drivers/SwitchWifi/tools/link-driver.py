#!/usr/bin/env python3
"""Link the Switch Lite ARM64 KMDF WiFiCx driver using the WDK class stubs."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--objects", type=Path, required=True, help="output directory from check-core.py")
parser.add_argument("--kit", type=Path, required=True, help="Windows Kits/10 root containing Lib")
parser.add_argument("--kit-version", default="10.0.26100.0")
parser.add_argument("--cc", default="clang")
parser.add_argument("--linker", default="lld-link")
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
km = args.kit / "Lib" / args.kit_version / "km" / "ARM64"
wdf = args.kit / "Lib/wdf/kmdf/ARM64/1.33"
root = Path(__file__).resolve().parents[3]
thunk = args.out / "kmdf_support.obj"
subprocess.run([args.cc, "--target=aarch64-pc-windows-msvc", "-ffreestanding", "-c",
                str(root / "tools/kmdf_support.c"), "-o", str(thunk)], check=True)
libraries = [wdf / "wdfdriverentry.lib", wdf / "wdfldr.lib",
             km / "wificx/1.0/wificxstub.lib", km / "netcx/kmdf/adapter/2.2/netadaptercxstub.lib",
             km / "wlan/2.0/WificxTLVGenParse.lib", km / "ndis.lib",
             km / "bufferoverflowfastfailk.lib", km / "libcntpr.lib", km / "ntoskrnl.lib", km / "hal.lib"]
driver = args.out / "switchwifi.sys"
subprocess.run([args.linker, "/nologo", "/driver", "/subsystem:native,10.0", "/machine:arm64",
                "/entry:FxDriverEntry", "/nodefaultlib", "/release", "/opt:ref", "/opt:icf",
                # Use the same kernel section protections as tools/Build-Driver.ps1.
                # The WDF/Cx bind records in .data are writable at load time.
                "/section:.text,ERP", "/section:.rdata,RP", "/section:.data,RWP",
                "/section:.pdata,RP", "/section:INIT,ERD", "/tsaware:no",
                "/integritycheck", "/timestamp:0", f"/out:{driver}",
                str(thunk), str(args.objects / "SwitchWifiTransport.lib"), *map(str, libraries)], check=True)
print(f"Unsigned ARM64 driver: {driver}")
