
# WinX

<img width="1536" height="1024" alt="winx" src="https://github.com/user-attachments/assets/6c031e07-31b2-4030-8395-54d8e745d450" />

UEFI firmware for NVIDIA Tegra210 (T210) platforms.

WinX is derived from [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg) and uses the 202402-EDK2 build.

## Status

Capable of booting from an SD card through Hekate using a BL31/BL33 firmware setup.

A Linux kernel with EFI stub support should boot when supplied with a compatible device tree for a T210-based device.

ACPI-based booting also supports Windows and Linux. Available device support depends on the firmware configuration and the drivers provided.

## Working Features

The following features are reported as working in the supplied project notes:

- USB keyboard input.
- SD card access.
- USB flash drive access.
- Display output.
- UEFI Shell memory access through `mm`.
- UEFI Shell device listing through `devices`.
- Filesystem mapping through `map`.
- Filesystem access through mappings such as `fs0:`.
- FAT32 filesystem access.

## Device Support

- **CPU services:** GIC and ARM Generic Timer.
- **Clocks:** Clock, reset, PLL, and related services.
- **Power management:** PMC, PMIC, regulators, and related components.
- **GPIO and pin multiplexing.**
- **MicroSD:** SDSC and SDHC support. SDXC has been probed and its partition table detected, but has not been extensively tested.
- **eMMC:** I don't have any plans to provide support at the moment.
- **Display and framebuffer:** Display output is working in the documented setup. The framebuffer path requires a compatible Coreboot implementation.
- **Sideband buttons:** Not yet registered as UEFI input devices.
- **UART:** 115200 baud, 8N1, through the right-side rail connector.
- **ACPI:** Platform device descriptions, including SD/MMC controllers.
- **RTC and time:** No real-time clock is available. The `date` and `time` commands report values based on the build time. Changes are lost after power-off.
- **SMBIOS:** Platform information and clock-measurement support.
- **Fan control:** Fan-control driver integration.
- **USB 2.0:** Available, but may stop working after a voltage or connection-state change, such as disconnecting a keyboard while charging.

## UEFI Shell Limitations

- **FAT32:** Supported. exFAT has not been tested.
- **RAM disk:** Not usable because the required driver is absent.
- **PCI:** Not usable because the PCI bus driver is absent.
- **Networking:** Not usable because the underlying networking drivers are absent.
- **USB network adapters:** No adapter driver is included.
- **ACPI View:** The `acpiview` command requires `UefiShellAcpiViewCommandLib` to be included in the shell build.

To enable `acpiview`, add the following entry to the shell configuration:

```text
NULL|ShellPkg/Library/UefiShellAcpiViewCommandLib/UefiShellAcpiViewCommandLib.inf
```

## Hekate Configuration

Add the following entry to `bootloader/hekate_ipl.ini` on the SD card:

```ini
[Windows ARM64]
l4t=1
boot_prefixes=switchroot/winx/
id=WINXHKT
```

Place the firmware files in the following directory:

```text
switchroot/
└── winx/
    ├── bl31.bin
    └── bl33.bin
```

The supplied project notes report using the `bl31.bin` image from LineageOS 22.2, specifically the `nx_tab` build.

Other LineageOS builds and Ubuntu images were mentioned as possible alternatives, but were not confirmed in the supplied notes.

## Windows Kernel Debugger Enablement

```powershell
bcdedit /store E:\EFI\Microsoft\Boot\BCD /set {default} debug on
bcdedit /store E:\EFI\Microsoft\Boot\BCD /dbgsettings serial debugport:1 baudrate:115200
```

Replace the BCD store path and serial port settings as necessary.

Connect the serial interface through the right-side rail connector to a compatible host and configure WinDbg for serial kernel debugging at 115200 baud.

## Planned / In-Progress

- EHCI / USB 3.0 host support.
- eMMC support?
- Sideband buttons as UEFI input devices.
- Mouse support.
- Network stack and USB network adapter drivers.
- Additional UEFI Shell commands and utilities, including `acpiview`, `dp`, `tftp`, `ifconfig`, and `ping`.

## Building

See [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg). The build environment and workflow are almost identical!

WinX is based on [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg), with additional T210-specific firmware integration and modifications.

Refer to Lumia950XLPkg for the general build workflow, and consult the WinX and NintendoSwitchPkg source trees for platform-specific configuration, dependencies, and build targets.

## Credits

- [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg) — Base firmware package.
- [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg) — Build environment reference.
- [EDK II](https://github.com/tianocore/edk2) — UEFI development framework.
- [Hekate](https://github.com/CTCaer/hekate) — Bootloader and configuration integration.
- [Switchroot](https://switchroot.org/) — Source of compatible BL31 firmware through Switchroot/LineageOS packages. BL31 is supplied separately and is not included in this repository.

## Disclaimer

WinX is experimental firmware. The working features listed above reflect the supplied project notes. Other functionality may be incomplete, limited, or untested. Operating-system compatibility and hardware behavior may vary by configuration.
