# T214 PSCI power and BL33 entry patches

The build tool applies patches 0001 through 0006 to LineageOS ATF revision `85081cab2050051ea6e93206ef0f80bc0676e613`. It adapts Switchroot B01 restart preparation and MAX77620 shutdown in the shared T210 platform, gated by the T210B01 chip check and `SWITCH_LITE_PMIC_RESET=1`. Only the SoC-specific helper is under `soc/t210/t214`.

The experimental sleep/context patches are isolated on [`sc7-test`](https://github.com/ruje0504-2/winx/tree/sc7-test). This branch applies the shutdown/restart patches and the BL33 entry correction below.

## Build

Use an AArch64 bare-metal GCC toolchain and GNU make. From the WinX source root:

```sh
git clone --single-branch --branch lineage-22.2 https://github.com/LineageOS/android_hardware_nintendo_arm-trusted-firmware.git atf
git -C atf checkout 85081cab2050051ea6e93206ef0f80bc0676e613
python3 Tools/build-t214-atf.py atf out/t214-atf --cross-prefix aarch64-none-elf-
python3 Tools/test-t214-pmic.py atf
```

The build tool accepts the pinned checkout or its source archive, applies the patches (or recognizes them as already applied), and writes `out/t214-atf/bl31.bin`. It uses a separate build directory to avoid reusing objects from the rejected default-address build. SOURCE_DATE_EPOCH and the build string are fixed; use the same compiler for byte-identical output.

The default build uses native PMC/Hekate restart and leaves fan tracing disabled. `--native-restart` selects the same default explicitly. `--restart-as-off` reproduces the earlier fallback policy, and `--trace-power-boundary` adds two fan pulses at final power-off entry for diagnosis. Native and fallback builds, with and without tracing, use separate build directories. [Current physical results](../../docs/hdh001-home/power-performance.md)

The parameters follow [Lineage's Nintendo NX recipe](https://github.com/LineageOS/android_device_nintendo_nx/blob/lineage-22.2/vendor/nx-vendor.mk):

```text
PLAT=tegra TARGET_SOC=t210 TZDRAM_BASE=0xFFF00000
RESET_TO_BL31=1 COLD_BOOT_SINGLE_CPU=1 PROGRAMMABLE_RESET_ADDRESS=1
ENABLE_STACK_PROTECTOR=none SDEI_SUPPORT=0
CRASH_REPORTING=1 ENABLE_ASSERTIONS=1 LOG_LEVEL=0 PLAT_LOG_LEVEL_ASSERT=0
```

The unmodified reset-to-BL31 entrypoint obtains Hekate's parameters through the existing IRAM handoff. Do not use the default Tegra link address. The script checks the ELF entry against `0xFFF00000` before exporting the binary. For recent binutils, it disables the new RWX ELF-segment warning for this older linker script; runtime memory permissions still come from TF-A's page tables.

## BL33 entry state

Patch `0005` masks asynchronous exceptions in the saved BL33 SPSR before the EL3 return on T210B01. Hekate supplies `SPSR_EL2T` with DAIF clear; the UEFI stub cannot mask an exception taken at that return boundary before PrePi installs vectors. The patch preserves the requested exception level, stack selection and other SPSR bits. It does not change GPU initialization or the PSCI shutdown/restart handlers.

The `lite-entry-r7` build and binary inspection pass: the B01 branch ORs `0x3c0` into the saved SPSR (`0x8` becomes `0x3c8`). The existing PMIC host tests also pass. On 2026-11-08, the owner reported that startup now appears stable after this update, with no new occurrence of the early black screen reported. This is an observed improvement during normal use; no repeated-boot stress count was collected.

## Validation and deployment state

Patch `0006` captures the Hekate entry index and list during early BL31 setup. Native Windows Restart encodes that selector as `REBOOT_REASON_SELF` and recomputes the BootROM RTC block checksum. Missing or out-of-range selectors keep the menu fallback. Hekate resolves the selector against the INI entries when it restarts. [Hekate L4T handoff](https://github.com/CTCaer/hekate/blob/master/bootloader/l4t/l4t.c) · [RTC selector format](https://github.com/CTCaer/hekate/blob/master/bdk/rtc/max77620-rtc.h)

The `lite-entry-r8` build passed the simulated restart selector and PMIC tests. On 2026-11-09 at 09:10 Pacific, a remotely requested Windows Restart produced a new Windows boot and SSH session with the original Windows INI and Hekate autoboot disabled. The owner confirmed that Restart goes directly to Windows and powering on after Shutdown opens Hekate. The deployed BL31 SHA256 is `c2d048aaba276b2bc8d4b3f565f89d765a9851a522ced0985e551558c7c9e104`. The test Lite retains the previous image as `bl31.r7.bin`.

R6 uses Switchroot's GPIO-based PMIC transfer path during final power-off. With SwitchBattery 1.0.0.8 and all optional device drivers enabled, the owner confirmed normal Windows shutdown on the R6 diagnostic build: two fan pulses followed by complete power removal. The normal native-reset build was then deployed with tracing disabled, and the owner confirmed that Windows Restart completed normally. [Power status](../../docs/hdh001-home/power-performance.md)

The focused host test checks PMC timings, BootROM commands and checksums, the RTC selector and both hardware-I2C and GPIO PMIC failure paths. The ARM64 binary inspection confirms that native restart reaches PMC reset. Build and simulated results are separate from the physical shutdown and restart observations.

The new helper is GPL-2.0-only, derived from Switchroot's PMC/PMIC power code and Hekate's I2C code; original copyright is retained in its source. The remaining ATF source retains its upstream notices.
