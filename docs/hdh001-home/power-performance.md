# HDH-001 shutdown and restart

The Home desktop now has owner-confirmed audible output and working brightness controls. Returned-card readback confirms active audio 1.0.0.9, fan 1.0.0.5 and a 512 MiB pagefile with a 2048 MB maximum. Shutdown and restart still need acceptance: the owner reports that shutdown turns the screen black without actually removing power.

## T214 PSCI power patch

The deployed Lineage ATF T210 SYSTEM_OFF handler only reports an unsupported operation and panics. Its reset handler uses PMC reset. The new [ATF patch](../../patches/atf/0001-t214-psci-pmic-power.patch) adds a T210B01-only MAX77620 path under the existing T210 platform, with the SoC helper in `t214/pmic_power.c`.

After Windows requests the final PSCI transition, the helper reinitializes I2C5. Shutdown clears soft-reset/alarm wake and issues PMIC soft reset; restart enables soft-reset wake before the same power-reset command. Other PMIC wake settings are preserved. The helper sends no regulator-voltage commands. I2C/configuration failures retain the existing platform fallback; the final command gets time to complete even if power loss prevents its I2C acknowledgement.

Restart is a full power cycle through the existing boot chain. Hekate's current autoboot configuration determines whether it displays the menu or launches an entry. The sole Windows entry remains **Lite Windows 11 Home**.

## Build contract and status

Use [the pinned source and build instructions](../../patches/atf/README.md). The Nintendo NX recipe requires `TZDRAM_BASE=0xFFF00000`, reset-to-BL31 startup and the IRAM handoff. The default Tegra link address is not compatible with this Hekate setup.

The initial R2 candidate used the default link address and different startup parameters. It failed before Windows and was withdrawn; the original working BL31 was restored. R3 uses Lineage's complete NX parameters and the unmodified upstream entrypoint. The build script rejects a mismatched ELF entry address. R3 compilation and simulated PMIC off/restart/I2C-failure checks pass; physical boot and power transitions are pending.

Only `switchroot/woa-lite-home/bl31.bin` changes. Preserve the original file for rollback. Windows setup, BCD, drivers, clocks and existing partitions are not changed by this firmware update.

## Existing BootROM preparation

`Drivers/PmicDxe/t214/LiteResetDxe.c` retains the eight-word BootROM command table used by Switchroot's Mariko device tree. Its encoding and block checksums match Linux, but the physical result showed that this preparation alone did not solve Windows restart. It remains in the existing Home UEFI; R3 changes BL31's final PSCI power action.

## Primary references

- [Lineage Nintendo NX ATF build parameters](https://github.com/LineageOS/android_device_nintendo_nx/blob/lineage-22.2/vendor/nx-vendor.mk)
- [Pinned Lineage T210 PSCI handlers](https://github.com/LineageOS/android_hardware_nintendo_arm-trusted-firmware/blob/85081cab2050051ea6e93206ef0f80bc0676e613/plat/nvidia/tegra/soc/t210/plat_psci_handlers.c)
- [Hekate PMIC power states](https://github.com/CTCaer/hekate/blob/master/bdk/utils/util.c)
- [Hekate I2C controller setup](https://github.com/CTCaer/hekate/blob/master/bdk/soc/i2c.c)
- [Switchroot Mariko BootROM commands](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi)
