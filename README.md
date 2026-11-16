<img width="1536" height="1024" alt="winx" src="https://github.com/user-attachments/assets/6c031e07-31b2-4030-8395-54d8e745d450" />

# WinX for Nintendo Switch Lite

Boot Windows 11 ARM64 from microSD on the **Nintendo Switch Lite (HDH-001, Tegra T214)**. On a prepared card, select **Windows 11 Lite** in Hekate. Windows 11 Home and a trimmed Pro installation both reached the desktop on build 22631.2861 (23H2).

## Hardware status

| Feature | Result on the tested Lite |
| --- | --- |
| microSD / SDR104 / DDR200 | **0.5.1.2 / `oem90.inf` / Code 0** verified after restart, including the active SYS hash and an 8 MiB write/flush/readback check. The new trace found isolated passing taps 30 and 77 in 128 samples with no timeout; the one-tap maximum window was rejected and SDR104 recovered. DDR200 remains inactive; actual crash-dump capture remains unverified. FAT32 and the 80 GiB `WINARM` layout are retained. [Storage evidence](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/Tegra210Sdhc/README.md#0512-installation-and-runtime-result) |
| Display and touch | Lite touch **1.0.1.2** is currently installed, with GPIO **1.0.0.4** and I²C **1.0.0.5**. The trimmed Pro desktop works in landscape; earlier FTM4 testing confirmed taps in Windows Setup. The current version inventory is not a new touch acceptance test. |
| Analog sticks and gamepad | SwitchSio **0.2.5.0** is currently installed. Windows previously detected the built-in HID gamepad/mouse and controller input. Both sticks are exposed through generic HID; in-game use awaits testing. |
| Desktop controls | Current installed package: **SwitchSio 0.2.5.0**. Left-stick pointer, ZR/ZL clicks, Capture desktop mode and **+** On-Screen Keyboard are the existing mapping. Right-stick scrolling, R3 middle click and D-pad arrows were added in 0.2.3; they have not been revalidated on 0.2.5.0. [Button mapping](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchSensor/README.md) |
| 3D graphics | Current experimental `.158 + bit14` tgd3d/NVK desktop activates5seconds after logon. DWM module loading is verified; stable landscape and nativevkcube remain pending. [GPU project](https://github.com/ruje0504-2/mesa-switch-woa/tree/tgd3d-vulkan) |
| x64 application emulation / FEX ARM64EC | **Possible future direction; not integrated or validated in the WinX baseline.** Evaluate the native Windows x64 CPU-emulation backend after desktop/landscape and native `vkcube` acceptance. Verify CPU/OS support, graphics DLL ABI compatibility, performance and rollback; ordinary ARM64 DLLs are not directly compatible with ARM64EC/x64 processes. The upstream native-Windows x86/WOW64 replacement is currently unsupported. [FEX Windows guide](https://wiki.fex-emu.com/index.php/Development:ARM64EC#Windows) · [ARM64EC compatibility](https://learn.microsoft.com/en-us/windows/arm/arm64ec#interoperability) |
| Wi-Fi and Bluetooth | **Wi-Fi 0.5.7.0** is owner-confirmed usable after the SD update on November 12. SSH confirms `oem88.inf`, Code 0 and the exact installed SYS hash. It checks firmware disconnect state before failing the adapter for a missing event and records the leave phase. Repeated reconnects and sustained stability remain pending. **Bluetooth 1.0.0.19** is installed and was owner-confirmed working. [Wi-Fi evidence and references](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchWifi/README.md) |
| Audio and backlight | **SwitchAudio 1.0.0.9** is installed. The built-in speakers play audio; brightness controls work on the internal panel. |
| RTC | **SwitchRtc 1.0.0.3** is installed. Earlier RTC/time restoration passed; the current inventory is not a new long-duration retention test. |
| Fan and clocks | **Fan33 installed and owner-tested.** Noble ACTMON IRQ plus CPU/display bandwidth floors; trained204/1331.2/1600/2500MHz RAM, independent saved power/fan choices. [Current acceptance](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/docs/hdh001/dvfs-tray.md) |
| Power button / S0 screen-off and wake | **SwitchPowerButton1.0.0.7** polling/core-voltage owner installed. Earlier short-button acceptance belongs to1.0.0.4. SC7 remains disabled; S4 remains unsupported. |
| Shutdown and restart | Current Lite battery package: **SwitchBattery 1.0.0.14**, including the request-ownership fix introduced in 1.0.0.8. The owner previously confirmed normal shutdown and native restart. BL31 r8 returns Windows Restart to the captured Windows entry; a cold boot after shutdown opens Hekate. [Power status](docs/hdh001-home/power-performance.md) |
| Charge termination / monitoring | **SwitchBattery1.0.0.14** retains termination/safety timer and persistent85%limit. Fresh battery net current/power is shown in Pacecar. Both tray tools auto-start; Pacecar background is50%transparent. [Evidence](docs/hdh001-home/power-performance.md) |

Package versions were re-read after the SD update on **2026-11-13 00:54 UTC**
(November 12 local time). Installation is
separate from feature acceptance. Default-branch source remains Lite touch
1.0.1.0 and SwitchSio0.2.3.0; the newer installed
test packages are identified above without claiming their source was promoted.
Wi-Fi remains **0.5.7.0**, with the installed hash matched to source; no 0.5.8
package was found in that inventory. The current GPU recovery pairing is the matched`.158`stack;`.59`is historical.
Native DWM/landscape stability remains under development in the GPU repository.
USB-C peripherals and headphones await hardware testing. [Porting status](docs/hdh001-home/PORTING.md)

Driver source is maintained in [Windows-for-Switch-Drivers](https://github.com/ruje0504-2/Windows-for-Switch-Drivers). The `WindowsDrivers` submodule pins the matching revision.

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
