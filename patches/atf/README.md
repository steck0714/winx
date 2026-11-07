# T214 PSCI PMIC power patch

The build tool applies patches 0001 through 0005 to LineageOS ATF revision `85081cab2050051ea6e93206ef0f80bc0676e613`. It adapts Switchroot B01 restart preparation and MAX77620 shutdown in the shared T210 platform, gated by the T210B01 chip check and `SWITCH_LITE_PMIC_RESET=1`. Only SoC-specific helpers are under `soc/t210/t214`.

## Build

Use an AArch64 bare-metal GCC toolchain and GNU make. From the WinX source root:

```sh
git clone --single-branch --branch lineage-22.2 https://github.com/LineageOS/android_hardware_nintendo_arm-trusted-firmware.git atf
git -C atf checkout 85081cab2050051ea6e93206ef0f80bc0676e613
python3 Tools/build-t214-atf.py atf out/t214-atf --cross-prefix aarch64-none-elf-
python3 Tools/test-t214-pmic.py atf
```

The build tool accepts the pinned checkout or its source archive, applies the patch (or recognizes it as already applied), and writes `out/t214-atf/bl31.bin`. It uses a separate build directory to avoid reusing objects from the rejected default-address build. SOURCE_DATE_EPOCH and the build string are fixed; use the same compiler for byte-identical output.

The default build uses native PMC/Hekate restart and leaves fan tracing disabled. `--native-restart` selects the same default explicitly. `--restart-as-off` reproduces the earlier fallback policy, and `--trace-power-boundary` adds two fan pulses at final power-off entry for diagnosis. Native and fallback builds, with and without tracing, use separate build directories. [Current physical results](../../docs/hdh001-home/power-performance.md)

Patch 0005 is disabled by default. The development option `--sc7` sets
`SWITCH_LITE_SC7=1` and uses a separate build directory. For B01 PSCI
`CPU_SUSPEND` system-powerdown requests, it requires a live BPMP, sends
`MRQ_ENABLE_SUSPEND` with `{ TEGRA_PM_SC7, 0 }`, and arms the board's active-low
PMIC wake event 51. BPMP errors or failed wake-mask readback reject entry before
PSCI commits to powerdown. Other wake masks, levels and pending events are
preserved. The paired Windows backend still needs complete device/context
coordination; this option alone is **not a working SC7 implementation**.

```sh
python3 Tools/build-t214-atf.py atf out/t214-sc7 --cross-prefix aarch64-none-elf- --sc7
python3 Tools/test-t214-sc7.py atf
```

The ARM64 development build and actual-validator/PMC fault tests pass. They
cover missing BPMP, rejected requests, failed wake-mask writes, repeat entry,
and the original non-B01/default paths. The candidate is not installed and has
no physical sleep/wake result. [Switchroot entry](https://github.com/CTCaer/switch-l4t-kernel-nvidia/blob/578d533b99cd5d5c3e57760431d2f621307265cf/drivers/soc/tegra/pm-tegra210.c#L68) · [PMC wake setup](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/soc/tegra/pmc.c#L1940)

The parameters follow [Lineage's Nintendo NX recipe](https://github.com/LineageOS/android_device_nintendo_nx/blob/lineage-22.2/vendor/nx-vendor.mk):

```text
PLAT=tegra TARGET_SOC=t210 TZDRAM_BASE=0xFFF00000
RESET_TO_BL31=1 COLD_BOOT_SINGLE_CPU=1 PROGRAMMABLE_RESET_ADDRESS=1
ENABLE_STACK_PROTECTOR=none SDEI_SUPPORT=0
CRASH_REPORTING=1 ENABLE_ASSERTIONS=1 LOG_LEVEL=0 PLAT_LOG_LEVEL_ASSERT=0
```

The unmodified reset-to-BL31 entrypoint obtains Hekate's parameters through the existing IRAM handoff. Do not use the default Tegra link address. The script checks the ELF entry against `0xFFF00000` before exporting the binary. For recent binutils, it disables the new RWX ELF-segment warning for this older linker script; runtime memory permissions still come from TF-A's page tables.

## Validation and deployment state

R6 uses Switchroot's GPIO-based PMIC transfer path during final power-off. With SwitchBattery 1.0.0.8 and all optional device drivers enabled, the owner confirmed normal Windows shutdown on the R6 diagnostic build: two fan pulses followed by complete power removal. The normal native-reset build was then deployed with tracing disabled, and the owner confirmed that Windows Restart completed normally. [Power status](../../docs/hdh001-home/power-performance.md)

The focused host test checks PMC timings, BootROM commands/checksums, the RTC Hekate-menu request and both hardware-I2C and GPIO PMIC failure paths. The ARM64 binary inspection confirms that native restart reaches PMC reset instead of the power-off handler. Build and simulated results are separate from the physical shutdown observation.

The new helper is GPL-2.0-only, derived from Switchroot's PMC/PMIC power code and Hekate's I2C code; original copyright is retained in its source. The remaining ATF source retains its upstream notices.
