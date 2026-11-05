# HDH-001 restart preparation

The physical Home boot completed setup phase 4 and windeploy returned 0, then requested a planned restart before OOBE. The panel went dark and the owner reported that Windows restarts do not recover. Preserve the completed installation state; OOBE remains pending.

Switchroot Ubuntu 5.1.2 and LineageOS 22.2 include a Mariko BootROM reset-command table. Linux's PMC driver programs it before reboot. The final PSCI/PMC chip reset then makes BootROM write RTC reboot metadata and issue a MAX77620 power reset. The Home firmware previously did not install this table.

`Drivers/PmicDxe/t214` installs the same eight words at PMC's BootROM command scratch area. It writes retained command storage only; the PMIC commands are executed by BootROM after a later reset. The driver is gated to T210B01, commits the header last and reads back the words. Enable it with `-D SWITCH_LITE_RESET=1` alongside the existing HDH-001 touch profile. Keep the existing BL31 and the **Lite Windows 11 Home** Hekate entry.

Validation: AARCH64 firmware build; all eight command words match the unmodified Linux encoder with the two DT command blocks; both block checksums are valid. This is a **hardware-untested restart candidate**, not proof that the missing table is the sole cause or that Windows reaches PSCI in every failed restart. There are no CPU, memory or GPU clock changes.

References:
- [Ubuntu Mariko reset commands](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi)
- [Android Mariko reset commands](https://github.com/LineageOS/android_hardware_nvidia_platform_t210_nx/blob/lineage-22.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi)
- [Linux PMC encoding and initialization](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/soc/tegra/pmc.c)
- [Linux restart sequence](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/firmware/psci.c)

SYSTEM_OFF is a separate gap in the current Switchroot ATF T210 implementation. A direct-PMIC PSCI experiment was compiled locally but is not part of this candidate.
