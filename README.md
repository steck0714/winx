<img width="1536" height="1024" alt="winx" src="https://github.com/user-attachments/assets/6c031e07-31b2-4030-8395-54d8e745d450" />

# WinX for Nintendo Switch Lite

Boot Windows 11 ARM64 from microSD on the **Nintendo Switch Lite (HDH-001, Tegra T214)**. On a prepared card, select **Windows 11 Lite** in Hekate. Windows 11 Home and a trimmed Pro installation both reached the desktop on build 22631.2861 (23H2).

## Hardware status

| Feature | Result on the tested Lite |
| --- | --- |
| Storage | Windows boots from the 80 GiB NTFS volume `WINARM`. Writes survived shutdown and a cold boot. [SDR104 / ADMA2](WindowsDrivers/drivers/Tegra210Sdhc/README.md#t214-sdr104-candidate) first trial failed to boot (`INACCESSIBLE_BOOT_DEVICE`); investigation is in progress. |
| Display and touch | The trimmed Pro desktop works in landscape. The FTM4 touchscreen accepts taps in Windows Setup. |
| Analog sticks and gamepad | Windows detected the built-in HID gamepad and mouse and the driver received controller input. Both sticks are exposed to games with generic controller support. In-game use awaits testing. |
| Desktop controls | Left stick moves the pointer and ZR/ZL click. Capture toggles desktop mode and **+** opens the On-Screen Keyboard. Version 0.2.3 adds right-stick scrolling, R3 middle click and D-pad keyboard arrows. Hardware test pending. [Button mapping](WindowsDrivers/drivers/SwitchSensor/README.md) |
| 3D graphics | Windows uses CPU software rendering. Native GM20B Vulkan acceleration is planned with Mesa NVK. [GPU driver status](docs/gpu-vulkan/STATUS.md) |
| Wi-Fi and Bluetooth | Wi-Fi 0.5.5 reconnected to a saved WPA2-Personal network, obtained an IPv4 address through DHCP and reached the Internet. Bluetooth works on the trimmed Pro installation. |
| Audio and backlight | The built-in speakers play audio from Windows. The brightness controls work on the internal panel. |
| Fan and clocks | The fan runs. Live readback showed 1996.8 MHz CPU and 1866 MHz RAM. |

On R5, shutting down Windows still leaves the screen black and the fan running until the owner holds the power button. Restart uses the same firmware handler and awaits a separate R5 test. USB-C peripherals and headphones await hardware testing. [Porting status](docs/hdh001-home/PORTING.md)

## Storage profiles

**WINARM only** is the default. In a comparison on the same Lite and card, the owner reported a noticeably smoother desktop with FAT32 hidden. Hekate continues to use the physical FAT32 partition.

**WINARM + FAT32** is available as an experimental build with `--expose-fat32`. Windows can mount both volumes, but the interface lagged in this configuration. The cause is still open. Build the firmware and Windows SD driver from the same generated header whenever you switch profiles. [Storage build instructions](docs/hdh001-home/README.md#firmware-build)

## Building and installing

Start with the [Stage 6 guide](docs/hdh001-home/README.md) to prepare the boot files and drivers. Use your own Windows ARM64 image and a compatible Switchroot BL31. Windows driver packages and their hardware results are in the [companion driver repository](https://github.com/ruje0504-2/Windows-for-Switch-Drivers).

## Credits

WinX builds on [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg) and [EDK II](https://github.com/tianocore/edk2), with the [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg) build workflow. Boot and board support draw on [Hekate](https://github.com/CTCaer/hekate), [Switchroot](https://switchroot.org/) and hayato41200’s [Windows-for-Switch firmware](https://github.com/hayato41200/Windows-for-Switch) and [drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers). For the original T210/Erista port, see the [upstream WinX notes](README.winx.md). [License](LICENSE)
