#!/usr/bin/env python3
"""Build the Lite PSCI power patch against the pinned Lineage ATF checkout."""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess

REVISION = "85081cab2050051ea6e93206ef0f80bc0676e613"
REPO = Path(__file__).resolve().parents[1]
PATCH = REPO / "patches/atf/0001-t214-psci-pmic-power.patch"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cross-prefix", default=os.environ.get("CROSS_COMPILE", "aarch64-none-elf-"))
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    source = args.source.resolve()
    if (source / ".git").exists():
        head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        if head != REVISION:
            raise SystemExit(f"Expected ATF revision {REVISION}; found {head}")
    check = subprocess.run(["git", "apply", "--check", str(PATCH)], cwd=source, capture_output=True)
    if check.returncode == 0:
        subprocess.run(["git", "apply", str(PATCH)], cwd=source, check=True)
    else:
        subprocess.run(["git", "apply", "--reverse", "--check", str(PATCH)], cwd=source, check=True)
    make = shutil.which("gmake") or shutil.which("make")
    if not make:
        raise SystemExit("GNU make is required")
    build_base = "build-lite-power-r3"
    # LineageOS android_device_nintendo_nx/vendor/nx-vendor.mk. Hekate loads
    # at 0xFFF00000 and supplies the reset-to-BL31 parameters through IRAM.
    flags = [make, f"-j{args.jobs}", "PLAT=tegra", "TARGET_SOC=t210",
             f"BUILD_BASE={build_base}", "TZDRAM_BASE=0xFFF00000",
             "RESET_TO_BL31=1", "COLD_BOOT_SINGLE_CPU=1",
             "PROGRAMMABLE_RESET_ADDRESS=1", "ENABLE_STACK_PROTECTOR=none",
             "SDEI_SUPPORT=0", "CRASH_REPORTING=1", "PLAT_LOG_LEVEL_ASSERT=0",
             f"CROSS_COMPILE={args.cross_prefix}", "DEBUG=0", "LOG_LEVEL=0",
             "ENABLE_ASSERTIONS=1", "SWITCH_LITE_PMIC_RESET=1",
             "BUILD_STRING=85081ca-lite-power-r3"]
    help_text = subprocess.check_output([args.cross_prefix + "ld", "--help"], text=True)
    if "--no-warn-rwx-segments" in help_text:
        # TF-A 2.6's ELF layout predates this binutils diagnostic. Runtime MMU
        # permissions come from TF-A's translation tables, not ELF LOAD flags.
        flags.append("LDFLAGS=--no-warn-rwx-segments")
    env = dict(os.environ, SOURCE_DATE_EPOCH="1793836800")
    subprocess.run(flags + ["bl31"], cwd=source, env=env, check=True)
    release = source / build_base / "tegra/t210/release"
    binary = release / "bl31.bin"
    elf_header = (release / "bl31/bl31.elf").read_bytes()[:64]
    if elf_header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<Q", elf_header, 24)[0] != 0xFFF00000:
        raise SystemExit("BL31 entry does not match Hekate's 0xFFF00000 load address")
    if binary.stat().st_size >= 0x40000:
        raise SystemExit("BL31 exceeds the platform's reserved image range")
    args.output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, args.output / "bl31.bin")
    print(f"Built {args.output / 'bl31.bin'}; physical power-off/restart not yet verified")


if __name__ == "__main__":
    main()
