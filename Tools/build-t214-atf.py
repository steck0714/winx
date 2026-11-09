#!/usr/bin/env python3
"""Build the Lite PSCI power and UEFI entry patches against pinned Lineage ATF."""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess

REVISION = "85081cab2050051ea6e93206ef0f80bc0676e613"
REPO = Path(__file__).resolve().parents[1]
POWER_PATCHES = [REPO / "patches/atf" / name for name in (
    "0001-t214-psci-pmic-power.patch",
    "0002-t214-windows-shutdown.patch",
    "0003-t214-shutdown-gpio-i2c.patch",
    "0004-t214-power-boundary-trace.patch",
    "0005-t214-bl33-entry.patch",
    "0006-t214-windows-restart-entry.patch",
)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cross-prefix", default=os.environ.get("CROSS_COMPILE", "aarch64-none-elf-"))
    parser.add_argument("--jobs", type=int, default=8)
    restart = parser.add_mutually_exclusive_group()
    restart.add_argument("--native-restart", dest="restart_as_off", action="store_false",
                         help="use the PMC/Hekate reset path (default)")
    restart.add_argument("--restart-as-off", action="store_true",
                         help="reproduce the earlier restart-to-power-off diagnostic policy")
    parser.set_defaults(restart_as_off=False)
    parser.add_argument("--trace-power-boundary", action="store_true",
                        help="diagnostic fan pulses at final PSCI entry; not a shutdown fix")
    args = parser.parse_args()
    source = args.source.resolve()
    if (source / ".git").exists():
        head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        if head != REVISION:
            raise SystemExit(f"Expected ATF revision {REVISION}; found {head}")
    def applied(patch):
        return subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                              cwd=source, capture_output=True).returncode == 0

    # Newer patches edit earlier hunks. Check the newest applied patch first.
    pending = next((i + 1 for i in reversed(range(len(POWER_PATCHES)))
                    if applied(POWER_PATCHES[i])), 0)
    for patch in POWER_PATCHES[pending:]:
        subprocess.run(["git", "apply", str(patch)], cwd=source, check=True)
    make = shutil.which("gmake") or shutil.which("make")
    if not make:
        raise SystemExit("GNU make is required")
    build_base = "build-lite-entry-r8-off" if args.restart_as_off else "build-lite-entry-r8-native"
    if args.trace_power_boundary:
        build_base += "-trace"
    # LineageOS android_device_nintendo_nx/vendor/nx-vendor.mk. Hekate loads
    # at 0xFFF00000 and supplies the reset-to-BL31 parameters through IRAM.
    flags = [make, f"-j{args.jobs}", "PLAT=tegra", "TARGET_SOC=t210",
             f"BUILD_BASE={build_base}", "TZDRAM_BASE=0xFFF00000",
             "RESET_TO_BL31=1", "COLD_BOOT_SINGLE_CPU=1",
             "PROGRAMMABLE_RESET_ADDRESS=1", "ENABLE_STACK_PROTECTOR=none",
             "SDEI_SUPPORT=0", "CRASH_REPORTING=1", "PLAT_LOG_LEVEL_ASSERT=0",
             f"CROSS_COMPILE={args.cross_prefix}", "DEBUG=0", "LOG_LEVEL=0",
             "ENABLE_ASSERTIONS=1", "SWITCH_LITE_PMIC_RESET=1",
             f"SWITCH_LITE_POWER_TRACE={int(args.trace_power_boundary)}",
             f"SWITCH_LITE_RESTART_AS_OFF={int(args.restart_as_off)}",
             "BUILD_STRING=85081ca-lite-entry-r8" + ("-trace" if args.trace_power_boundary else "")]
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
    print(f"Built {args.output / 'bl31.bin'}; physical results: docs/hdh001-home/power-performance.md")


if __name__ == "__main__":
    main()
