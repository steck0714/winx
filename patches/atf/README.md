# T214 PSCI PMIC power patch

Apply `0001-t214-psci-pmic-power.patch` to LineageOS ATF revision `85081cab2050051ea6e93206ef0f80bc0676e613`. It adds Lite shutdown/restart power control in the shared T210 platform, gated by the T210B01 chip check and `SWITCH_LITE_PMIC_RESET=1`. Only the PMIC-specific helper is under `soc/t210/t214`.

## Build

Use an AArch64 bare-metal GCC toolchain and GNU make. From the WinX source root:

```sh
git clone --single-branch --branch lineage-22.2 https://github.com/LineageOS/android_hardware_nintendo_arm-trusted-firmware.git atf
git -C atf checkout 85081cab2050051ea6e93206ef0f80bc0676e613
python3 Tools/build-t214-atf.py atf out/t214-atf --cross-prefix aarch64-none-elf-
python3 Tools/test-t214-pmic.py atf
```

The build tool accepts the pinned checkout or its source archive, applies the patch (or recognizes it as already applied), and writes `out/t214-atf/bl31.bin`. It uses a separate build directory to avoid reusing objects from the rejected default-address build. SOURCE_DATE_EPOCH and the build string are fixed; use the same compiler for byte-identical output.

The parameters follow [Lineage's Nintendo NX recipe](https://github.com/LineageOS/android_device_nintendo_nx/blob/lineage-22.2/vendor/nx-vendor.mk):

```text
PLAT=tegra TARGET_SOC=t210 TZDRAM_BASE=0xFFF00000
RESET_TO_BL31=1 COLD_BOOT_SINGLE_CPU=1 PROGRAMMABLE_RESET_ADDRESS=1
ENABLE_STACK_PROTECTOR=none SDEI_SUPPORT=0
CRASH_REPORTING=1 ENABLE_ASSERTIONS=1 LOG_LEVEL=0 PLAT_LOG_LEVEL_ASSERT=0
```

The unmodified reset-to-BL31 entrypoint obtains Hekate's parameters through the existing IRAM handoff. Do not use the default Tegra link address. The script checks the ELF entry against `0xFFF00000` before exporting the binary. For recent binutils, it disables the new RWX ELF-segment warning for this older linker script; runtime memory permissions still come from TF-A's page tables.

## Validation and deployment state

The focused host test simulates I2C/PMIC register transactions. It checks shutdown versus restart wake policy, configuration-read failure, configuration-readback failure, and a missing acknowledgement on the final reset command. It does not simulate actual rail power or establish that Windows reaches PSCI.

R2 failed before Windows because its build did not follow the NX address/startup contract. It was withdrawn and the known-good BL31 restored. R3 has been rebuilt with the official parameters; physical boot, actual power-off and restart remain pending. Deployment replaces only the Home `bl31.bin` and retains its backup and the existing Hekate entry. See [power status](../../docs/hdh001-home/power-performance.md).

The new helper is GPL-2.0-only, derived from Hekate's I2C and power-state code; original copyright is retained in its source. The remaining ATF source retains its upstream notices.
