<img width="1536" height="1024" alt="winx" src="https://github.com/user-attachments/assets/6c031e07-31b2-4030-8395-54d8e745d450" />

# WinX for Nintendo Switch Lite

Boot Windows 11 ARM64 from microSD on the **Nintendo Switch Lite (HDH-001, Tegra T214)**. On a prepared card, select **Windows 11 Lite** in Hekate. Windows 11 Home and a trimmed Pro installation both reached the desktop on build 22631.2861 (23H2).

The hardware results below are owner-reported physical tests on the stated Lite and driver revisions. They do not establish that the current WinX UEFI image or other Switch models behave the same way. Build/QEMU checks and source-derived theoretical values are labeled separately in the [verification-status guide](docs/verification-status.md).

## Hardware status

| Feature | Result on the tested Lite |
| --- | --- |
| microSD / SDR104 / DDR200 | Driver **0.5.0.3** boots Windows from the 80 GiB `WINARM` volume with FAT32 visible; the owner confirmed desktop boot. DDR200 automatic detection, tuning and SDR104 recovery are implemented. Active DDR200 mode and throughput still need runtime confirmation. [Storage status](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/Tegra210Sdhc/README.md#t214-ddr200-candidate-0501) |
| Display and touch | The trimmed Pro desktop works in landscape. The FTM4 touchscreen accepts taps in Windows Setup. |
| Analog sticks and gamepad | Windows detected the built-in HID gamepad and mouse and the driver received controller input. Both sticks are exposed to games with generic controller support. In-game use awaits testing. |
| Desktop controls | Left stick moves the pointer and ZR/ZL click. Capture toggles desktop mode and **+** opens the On-Screen Keyboard. Version 0.2.3 adds right-stick scrolling, R3 middle click and D-pad keyboard arrows. Hardware test pending. [Button mapping](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchSensor/README.md) |
| 3D graphics | Windows still uses CPU software rendering. The source has a T214 GPU probe/backend experiment, but no finished WDDM display miniport or Vulkan ICD; the T210 GM20B diagnostic is only S0–S2. A separate project reports an off-screen Vulkan result, not WinX game support. [GPU readiness](docs/driver-gap-audit-ja.md) |
| Wi-Fi and Bluetooth | Wi-Fi 0.5.5 reconnected to a saved WPA2-Personal network, obtained an IPv4 address through DHCP and reached the Internet. The owner confirmed Bluetooth working with 1.0.0.19 after the SD 0.5.0.3 update. |
| Audio and backlight | The built-in speakers play audio from Windows. The brightness controls work on the internal panel. |
| Fan and clocks | The fan runs. Live readback showed 1996.8 MHz CPU and 1866 MHz RAM. |
| Power button / S0 screen-off and wake | **switchpwrbtn 1.0.0.4** passed repeated power-button screen-off/wake on the Lite; the owner confirmed normal display and input after wake. SC7 deep sleep and S4 hibernation remain unsupported. [Tested driver source](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/tree/hdh001-winarm-bounded-write/drivers/SwitchPowerButton) |
| Shutdown and restart | The owner confirmed complete power-off from normal Windows shutdown and successful native restart. SwitchBattery 1.0.0.8 fixes battery request ownership; the current R6 firmware uses PMC reset and disables diagnostic fan pulses. [Power status](docs/hdh001-home/power-performance.md) |
| Charge termination | SwitchBattery **1.0.0.11** enables BQ24193 automatic termination and an eight-hour safety timer while preserving existing charging limits. Live configuration readback passed; a full-charge/recharge cycle remains unverified. [Charging policy](docs/hdh001-home/power-performance.md#charge-termination) |

USB-C peripherals and headphones await hardware testing. [Porting status](docs/hdh001-home/PORTING.md)

Windows driver source is vendored under `WindowsDrivers/t210/Icosa/` and `WindowsDrivers/t214/Hoag/`, with the GM20B experiment kept under the T210 tree. The former `WindowsDrivers` submodule pointer referenced an upstream commit that GitHub did not resolve from the fork; carrying the source here removes that broken navigation and build dependency. Provenance, licenses and validation limits are recorded in [WindowsDrivers/README.md](WindowsDrivers/README.md) and [NOTICE.winx.md](NOTICE.winx.md).

## Storage profiles

Both **WINARM only** and **WINARM + FAT32** are supported build profiles.
The current card exposes FAT32 using `--expose-fat32`. The latest owner
comparison found no clear responsiveness difference between the two views;
earlier reports of stalls do not establish a confirmed cause or a completed
performance fix. Hekate uses the physical FAT32 partition in either profile.

Build BL33 and the Windows SD driver from the same generated header whenever
you change the view. [Storage build instructions](docs/hdh001-home/README.md#firmware-build)

## Building and installing

Start with the [Stage 6 guide](docs/hdh001-home/README.md) to prepare the boot files and drivers. Use your own Windows ARM64 image and a compatible Switchroot BL31. Windows driver packages and their hardware results are in the [companion driver repository](https://github.com/ruje0504-2/Windows-for-Switch-Drivers).

For an experimental multi-OS menu that can chainload Windows and boot other AArch64 Linux kernels with a matching Switch DTB, see the [ARM64 Linux/GRUB guide](docs/multiboot/arm64-linux-ja.md). `SWITCH_GRUB_MENU` optionally embeds the GRUB EFI binary; Linux kernel, DTB and initramfs remain distro/SKU-specific and have not been hardware-validated here.

See also the [EfiFs filesystem and OS capability matrix](docs/multiboot/efifs-os-capability-ja.md) and [iPadOS/XNU integration limits](docs/multiboot/ipados-xnu-ja.md). EfiFs provides filesystem access, not an OS kernel loader; WinX has no verified XNU loader or APFS boot path.

Preboot controls, screenshots, and WinX-only splash/wallpaper files are tracked in [UEFI input and screenshot support](docs/preboot-input-ja.md), [UEFI audio/network/Bluetooth status](docs/preboot-media-ja.md), and [WinX UEFI visual customization](docs/winx-visuals-ja.md).

The landscape UEFI selector, separate entries for multiple installs of one OS, and the read-only custom EFI manifest are described in [WinX multi-boot](docs/winx-multiboot-ja.md). The model-by-model UEFI/Windows driver gaps, GM20B gaming readiness and microSD speed/safety status are in the [driver gap audit](docs/driver-gap-audit-ja.md).

Source is grouped by Switch hardware code name (`Platforms/Switch/t210/Icosa/` and `Platforms/Switch/t214/{Iowa,Hoag,Aula}/`) and by OS (`OperatingSystems/{Windows,Linux,iPadOS,Other}`). The [board/source map](docs/switch-model-source-layout-ja.md) explains which profiles have tested hardware paths and which are identification/layout scaffolding only. Hoag-specific ACPI resources are split from common Windows devices; this source organization does not imply that Iowa/Aula peripherals or every OS driver are hardware-validated.

## Credits

WinX builds on [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg) and [EDK II](https://github.com/tianocore/edk2), with the [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg) build workflow. Boot and board support draw on [Hekate](https://github.com/CTCaer/hekate), [Switchroot](https://switchroot.org/), [Switch-Linux device trees](https://github.com/shinyquagsire23/Switch-Linux), hayato41200’s [Windows-for-Switch firmware](https://github.com/hayato41200/Windows-for-Switch) and T210 driver snapshot, and the T214 Hoag driver snapshot from [ruje0504-2/Windows-for-Switch-Drivers](https://github.com/ruje0504-2/Windows-for-Switch-Drivers) at local source revision `ebdddd8`. The separately licensed GM20B diagnostic experiment retains its own notices.

Read-only filesystem EFI drivers come from [EfiFs v1.13](https://github.com/pbatard/efifs) (GPL-3.0-or-later; source pinned by the `efifs` submodule; see [filesystem driver notices](README.fsdrv)). The optional multi-OS EFI loader is built with [GNU GRUB](https://www.gnu.org/software/grub/); its GRUB modules and config are separate from EfiFs filesystem protocols.

iPadOS boot research references [ios-on-switch-lmao](https://github.com/brunostjohn/ios-on-switch-lmao) (GPL-2.0-only), [Apple XNU](https://github.com/apple-oss-distributions/xnu), Hekate and Switchroot. The WinX iPadOS notes use them as references and do not vendor that project's source or binaries. See the [iPadOS integration and per-model status](docs/multiboot/ipados-xnu-ja.md). For the original T210/Erista port, see the [upstream WinX notes](README.winx.md). [License](LICENSE)
