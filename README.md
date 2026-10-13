# WinX

UEFI firmware for NVIDIA Tegra210 (T210) platforms.

## Status

Capable of booting from SD card. A Linux kernel with EFI stub support should boot when supplied with a compatible device tree for a T210-based device.

ACPI-based booting also supports Windows and Linux, but available device support is currently limited, with CPU support being the primary implementation.

WinX is based on [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg), with additional firmware integration and platform-specific modifications.

## Device Support

- **CPU services:** GIC and ARM Generic Timer.
- **Clocks:** Clock, reset, PLL, and related services.
- **Power management:** PMC, PMIC, regulators, and related components.
- **GPIO and pin multiplexing.**
- **MicroSD:** SDSC and SDHC support. SDXC has been probed and its partition table detected, but has not been extensively tested.
- **eMMC:** Support is in progress.
- **Display and framebuffer:** Requires a compatible [Coreboot](https://github.com/imbushuo/Coreboot) implementation.
- **Sideband buttons:** Not yet registered as UEFI input devices.
- **UART:** 115200 baud, 8N1, through the right-side rail connector.
- **ACPI:** Platform device descriptions, including SD/MMC controllers.
- **RTC:** Virtual RTC support.
- **SMBIOS:** Platform information and clock-measurement support.
- **Fan control:** Fan-control driver integration.
- **USB host:** EHCI support planned.

Hardware support is incomplete. Component availability does not necessarily imply successful operation on physical hardware.

## Windows Kernel Debugger Enablement

```powershell
bcdedit /store E:\EFI\Microsoft\Boot\BCD /set {default} debug on
bcdedit /store E:\EFI\Microsoft\Boot\BCD /dbgsettings serial debugport:1 baudrate:115200
```

Replace the BCD store path and serial port settings as necessary.

Connect the serial interface to a compatible host and configure WinDbg for serial kernel debugging at 115200 baud.

## Planned / In-Progress

- EHCI USB host support.
- eMMC support.
- Sideband buttons as UEFI input devices.
- High-speed serial support for additional peripheral integration.

## Building

See [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg). The build environment and workflow are almost identical!

WinX is based on [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg), with additional T210-specific firmware integration and modifications.

Refer to Lumia950XLPkg for the general build workflow, and consult the WinX and NintendoSwitchPkg source trees for platform-specific configuration, dependencies, and build targets.

## Credits

- [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg) — Base firmware package.
- [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg) — Build environment reference.
- [EDK II](https://github.com/tianocore/edk2) — UEFI development framework.
- [Hekate](https://github.com/CTCaer/hekate) — Reference for platform naming and bootloader conventions.

## Disclaimer

WinX is experimental firmware. Hardware support and operating-system compatibility are not guaranteed. Features that have not been extensively tested should be considered experimental.
