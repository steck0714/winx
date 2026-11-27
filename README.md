<img width="1536" height="1024" alt="winx" src="https://github.com/user-attachments/assets/6c031e07-31b2-4030-8395-54d8e745d450" />

# WinX for Nintendo Switch Lite

WinX provides the UEFI firmware, ACPI tables and microSD boot profiles for Windows 11 ARM64 on **Nintendo Switch Lite (HDH-001, Tegra T214)**. On a prepared card, select **Windows 11 Lite** in Hekate. Windows 11 Home and trimmed Pro both reached the desktop on build 22631.2861 (23H2).

The standard Lite firmware profile uses Microsoft Basic Display. The accelerated desktop and video results below use the companion Mesa development stack. [Firmware build](docs/hdh001-home/README.md#firmware-build) · [Tested GPU components](https://github.com/ruje0504-2/mesa-switch-woa/blob/1cf6db16e13c34de39658641b68cdf71cabc292d/tegra-x1-wddm/docs/NVDEC_1080_D3D11VA_20261117.json)

## Hardware status

| Feature | Result on the tested Lite |
| --- | --- |
| microSD / SDR104 / DDR200 | **0.5.1.2 / `oem90.inf` / Code 0** verified after restart, including the active SYS hash and an 8 MiB write/flush/readback check. The new trace found isolated passing taps 30 and 77 in 128 samples with no timeout; the one-tap maximum window was rejected and SDR104 recovered. DDR200 remains inactive; actual crash-dump capture remains unverified. FAT32 and the 80 GiB `WINARM` layout are retained. [Storage evidence](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/Tegra210Sdhc/README.md#0512-installation-and-runtime-result) |
| Internal panel | Fixed 90° landscape was verified on the panel with GPU prerotation and row scan. Windows retained a 1280×720 desktop across restart on the native 720×1280 panel. DC column scan and all four orientation transitions await validation. [Display evidence](https://github.com/ruje0504-2/mesa-switch-woa/blob/1cf6db16e13c34de39658641b68cdf71cabc292d/tegra-x1-wddm/docs/ROW_SCAN_BASELINE_20261116.md) |
| Touch | Lite touch 1.0.1.2 was installed alongside GPIO 1.0.0.4 and I²C 1.0.0.5. Earlier FTM4 tests confirmed taps in Windows Setup. |
| Analog sticks and gamepad | SwitchSio **0.2.5.0** is currently installed. Windows previously detected the built-in HID gamepad/mouse and controller input. Both sticks are exposed through generic HID; in-game use awaits testing. |
| Desktop controls | Current installed package: **SwitchSio 0.2.5.0**. Left-stick pointer, ZR/ZL clicks, Capture desktop mode and **+** On-Screen Keyboard are the existing mapping. Right-stick scrolling, R3 middle click and D-pad arrows were added in 0.2.3; they have not been revalidated on 0.2.5.0. [Button mapping](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchSensor/README.md) |
| Accelerated desktop and Win32 Vulkan | The earlier fixed-landscape build passed DWM window interaction. Native ARM64 `vkcube` rendered through NVK for 300.289 seconds and exited normally. Resize and recovery from occlusion were confirmed on the panel. [Win32 Vulkan run](https://github.com/ruje0504-2/mesa-switch-woa/blob/1cf6db16e13c34de39658641b68cdf71cabc292d/tegra-x1-wddm/docs/VKCUBE_ACCEPTANCE_20261116.md) |
| H.264 video | D3D11VA/NVDEC passed paced six-frame window tests at 720p and 1080p on their respective GPU builds. All six frames in each run matched the decode reference. The 1080p window closed to the desktop. Sustained playback and load-related restarts remain open. [720p run](https://github.com/ruje0504-2/mesa-switch-woa/blob/1cf6db16e13c34de39658641b68cdf71cabc292d/tegra-x1-wddm/docs/NVDEC_720_COMPOSITOR_OPEN_20261117.json) · [1080p run](https://github.com/ruje0504-2/mesa-switch-woa/blob/1cf6db16e13c34de39658641b68cdf71cabc292d/tegra-x1-wddm/docs/NVDEC_1080_D3D11VA_20261117.json) |
| Direct3D games | PAL4 now starts and reaches a scene after the GM20B paging fix; Warcraft III reaches a map through NVK, with approximately 12 FPS visible in the owner's photo. Performance still needs optimization, and PAL4 game audio remains unresolved. [Game results and photo](docs/gpu-vulkan/GAMES_20261127.md) |
| x64 application emulation / FEX ARM64EC | FEX is a possible x64 emulation path if Prism is unavailable. WinX baseline integration and game validation remain open. An x64 game needs a compatible loader and graphics DLL architecture. [FEX Windows guide](https://wiki.fex-emu.com/index.php/Development:ARM64EC#Windows) · [ARM64EC compatibility](https://learn.microsoft.com/en-us/windows/arm/arm64ec#interoperability) |
| Wi-Fi and Bluetooth | **Wi-Fi 0.5.7.0** is owner-confirmed usable after the SD update on November 12. SSH confirms `oem88.inf`, Code 0 and the exact installed SYS hash. It checks firmware disconnect state before failing the adapter for a missing event and records the leave phase. Repeated reconnects and sustained stability remain pending. **Bluetooth 1.0.0.19** is installed and was owner-confirmed working. [Wi-Fi evidence and references](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchWifi/README.md) |
| Audio and backlight | **SwitchAudio 1.0.0.9** is installed. The built-in speakers play audio; brightness controls work on the internal panel. |
| RTC | **SwitchRtc 1.0.0.3** is installed. Earlier RTC/time restoration passed; the current inventory is not a new long-duration retention test. |
| Fan and clocks | Fan 1.0.0.33 power tests covered ACTMON interrupts and the trained 204, 1331.2, 1600 and 2500 MHz memory rates. The later 1080p GPU baseline recorded Handheld mode, CPU DFLL active, fan mode 1 and zero EMC error. [Power tests](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/docs/hdh001/dvfs-tray.md) · [1080p readback](https://github.com/ruje0504-2/mesa-switch-woa/blob/1cf6db16e13c34de39658641b68cdf71cabc292d/tegra-x1-wddm/docs/NVDEC_1080_D3D11VA_20261117.json) |
| Power button / S0 screen-off and wake | SwitchPowerButton 1.0.0.7 provides polling and core-voltage control. The earlier short-button wake result used 1.0.0.4. SC7 remains disabled. S4 remains unsupported. |
| Shutdown and restart | Current Lite battery package: **SwitchBattery 1.0.0.14**, including the request-ownership fix introduced in 1.0.0.8. The owner previously confirmed normal shutdown and native restart. BL31 r8 returns Windows Restart to the captured Windows entry; a cold boot after shutdown opens Hekate. [Power status](docs/hdh001-home/power-performance.md) |
| Charge termination and monitoring | SwitchBattery 1.0.0.14 retains charge termination, the safety timer and the 85% charge target. Pacecar shows battery net current and power. Both tray tools start at logon. [Power evidence](docs/hdh001-home/power-performance.md) |

The November 12–13 board inventory recorded Wi-Fi 0.5.7.0, touch 1.0.1.2 and SwitchSio 0.2.5.0. The default driver sources for touch and SwitchSio were 1.0.1.0 and 0.2.3.0 respectively. USB-C peripherals and headphones still await hardware testing. [Board porting status](docs/hdh001-home/PORTING.md)

`WindowsDrivers` pins the board-driver source used by this firmware checkout. Mesa tracks the graphics components independently, including the accepted 720p recovery combination for the November 17 GPU baseline. [Windows board drivers](https://github.com/ruje0504-2/Windows-for-Switch-Drivers) · [GPU component and recovery hashes](https://github.com/ruje0504-2/mesa-switch-woa/blob/1cf6db16e13c34de39658641b68cdf71cabc292d/tegra-x1-wddm/docs/NVDEC_1080_D3D11VA_20261117.json)

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

## Credits

WinX builds on [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg) and [EDK II](https://github.com/tianocore/edk2), with the [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg) build workflow. Boot and board support draw on [Hekate](https://github.com/CTCaer/hekate), [Switchroot](https://switchroot.org/) and hayato41200’s [Windows-for-Switch firmware](https://github.com/hayato41200/Windows-for-Switch) and [drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers). For the original T210/Erista port, see the [upstream WinX notes](README.winx.md). [License](LICENSE)
