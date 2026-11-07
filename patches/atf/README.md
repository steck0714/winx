# T214 PSCI PMIC power patch

The build tool applies `0001-t214-psci-pmic-power.patch` and `0002-t214-windows-shutdown.patch` to LineageOS ATF revision `85081cab2050051ea6e93206ef0f80bc0676e613`. It adapts Switchroot B01 restart preparation and MAX77620 shutdown in the shared T210 platform, gated by the T210B01 chip check and `SWITCH_LITE_PMIC_RESET=1`. Only the SoC-specific helper is under `soc/t210/t214`.

## Build

Use an AArch64 bare-metal GCC toolchain and GNU make. From the WinX source root:

```sh
git clone --single-branch --branch lineage-22.2 https://github.com/LineageOS/android_hardware_nintendo_arm-trusted-firmware.git atf
git -C atf checkout 85081cab2050051ea6e93206ef0f80bc0676e613
python3 Tools/build-t214-atf.py atf out/t214-atf --cross-prefix aarch64-none-elf-
python3 Tools/test-t214-pmic.py atf
```

The build tool accepts the pinned checkout or its source archive, applies the patch (or recognizes it as already applied), and writes `out/t214-atf/bl31.bin`. It uses a separate build directory to avoid reusing objects from the rejected default-address build. SOURCE_DATE_EPOCH and the build string are fixed; use the same compiler for byte-identical output.

R5 routes a B01 restart request through full power-off in this Windows BL31. To build the earlier native PMC/Hekate restart behavior for comparison, pass `--native-restart`. The primary and recovery PMIC paths use a separate output tree for each restart policy. [Windows shutdown investigation](../../docs/hdh001-home/power-performance.md#windows-power-off-fallback-r5)

The parameters follow [Lineage's Nintendo NX recipe](https://github.com/LineageOS/android_device_nintendo_nx/blob/lineage-22.2/vendor/nx-vendor.mk):

```text
PLAT=tegra TARGET_SOC=t210 TZDRAM_BASE=0xFFF00000
RESET_TO_BL31=1 COLD_BOOT_SINGLE_CPU=1 PROGRAMMABLE_RESET_ADDRESS=1
ENABLE_STACK_PROTECTOR=none SDEI_SUPPORT=0
CRASH_REPORTING=1 ENABLE_ASSERTIONS=1 LOG_LEVEL=0 PLAT_LOG_LEVEL_ASSERT=0
```

The unmodified reset-to-BL31 entrypoint obtains Hekate's parameters through the existing IRAM handoff. Do not use the default Tegra link address. The script checks the ELF entry against `0xFFF00000` before exporting the binary. For recent binutils, it disables the new RWX ELF-segment warning for this older linker script; runtime memory permissions still come from TF-A's page tables.

## Validation and deployment state

The focused host test simulates PMC and I2C/PMIC registers. It decodes the BootROM blocks, checks their byte checksums and the RTC Hekate-menu request, verifies the active-high PMC requests and NX timers, and confirms that restart preparation performs no live I2C rail writes. Shutdown tests cover interrupt-latch reads, configuration preservation and I2C failures. These checks do not emulate physical power or establish that Windows reaches PSCI.

R2 failed before Windows because it did not follow the NX startup contract. R3 boots but its owner-tested restart still leaves a black screen with the fan running; R3 shutdown was not tested. R4 replaces the direct PMIC restart attempt with Switchroot's PMC/BootROM route and preserves the R3 entrypoint/build contract. R4 is built and deployed. The owner confirms Windows boots but restart still black-screens with the fan running. R4 shutdown also fails with the fan running; the owner reports a recognizable Windows sound after the screen goes black. A subsequent pre-Windows shutdown using the same R4 BL31 powered off normally; Windows power actions remain unresolved, and pre-Windows restart is untested. See [power status](../../docs/hdh001-home/power-performance.md).

The new helper is GPL-2.0-only, derived from Switchroot's PMC/PMIC power code and Hekate's I2C code; original copyright is retained in its source. The remaining ATF source retains its upstream notices.
