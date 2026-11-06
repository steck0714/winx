# Switch Lite HDH-001

The Lite build uses the existing Tegra210Gpio and Tegra210I2c drivers and the FTM4 implementation in `drivers/SwitchLiteTouch`. The latter matches ACPI SWOA0502, initializes the controller in Windows, handles quick press/release events and retries HID submission. The public desktop checker is built from `touch-check.c`. Real-hardware testing has confirmed a touch press on the Windows setup dialog. The three-circle TOUCH PASS test is pending.

The build script also produces the existing SwitchPowerButton driver for the optional Lite I2C5/PMIC firmware profile. It reads MAX77620 ONOFFSTAT and ONOFFIRQ and submits HID System Power Down input. Use the firmware build with SWITCH_LITE_POWER=1 and stage switchpwrbtn.inf together with the I2C driver. The package compiles for ARM64; its physical button behavior is pending.

The companion Home firmware provides the GPIO interrupt resources, I²C3, touch power initialization and NTFS boot path. Its Hekate entry is **Lite Windows 11 Home**. [Firmware source](https://github.com/ruje0504-2/winx/tree/hdh001-hekate-sd)

## Compile the Lite driver set

Set WDK_INCLUDE to the WDK Include/version directory, WDK_LIB to Lib/version/km/ARM64, WDF_INCLUDE to Include/wdf/kmdf/1.15, and WDF_LIB to Lib/wdf/kmdf/ARM64/1.15. CLANG_CL and LLD_LINK select the tools. WDK_EXTRA_LIB can select the matching SPB, GPIO, VHF, and battery-class (`battc.lib`) libraries. The script builds GPIO, I²C, Lite touch, power button, fan, battery, RTC, and Bluetooth.

```sh
sh docs/hdh001/build-drivers.sh . /path/to/output
python3 docs/hdh001/tests/test-ftm4.py
```

The existing `tools/Build-Driver.ps1` also accepts the Lite source via its Sources argument and VHF libraries via Libs. Sign the final SYS and catalog files with the same development certificate used by the target Windows image. Install the GPIO, I²C and touch packages into the offline ARM64 image with DISM, then place the checker at `Users/Public/Desktop/LiteTouchCheck.exe`. Add the power-button package when using the optional power-enabled firmware.

## SD system partition

The bounded-write profile is in `drivers/Tegra210Sdhc/winarm`. Use the existing target generator with a saved image to produce the private header, and build the optional WINARM profile as described in the SD README. The matching INF is version 0.3.2.5.

The returned in-memory MBR places the configured WINARM partition in slot 1 and sets its active flag. This lets the system-partition check used by Windows Setup accept the already-booted MBR volume. The physical card's identifiers and full write bounds remain part of the selected profile's checks.

For the interrupted Home installation tested here, Windows subsequently required its failed specialize state to be restored before retry. The firmware repository records those setup logs and the recovery steps.

## Shared fan and T214 clock control

`drivers/SwitchFan` now supports both the original SWOA0401 device and Lite SWOA0402. Its common fan/I2C/backlight code stays in the upstream source file; only the Mariko clock helper and handoff definition are under `src/t214`. The original device retains its backlight interface. Older Lite firmware exposes PWM1 only; the new optional backlight profile adds a separate PWM0 resource and enables the existing brightness interface.

On Lite, TMP451 uses the Mariko -12.5 C remote offset. The optional firmware handoff requests CPU 1996.8 MHz with MAX77812 M4 at 1120 mV, independently of whether the firmware completed the requested EMC change. The fan timer owns both cooling and CPU transitions. At 80 C or after repeated sensor-read failures it restores the saved boot CPU frequency; at 70 C it can resume the request. It does not touch MAX77812 M1/GPU registers.

Use the matching firmware's `cpu_oc`/`ram_oc` INI options and its LitePerformanceCheck application. Parameters records include CpuActualKhz, RamActualKhz, TempSoC, TempBoard, Duty, SensorFailures and status. Build with `docs/hdh001/build-drivers.sh`; the shared INF is version 1.0.0.5. The existing Erista CPU-clock driver is not loaded for the Lite profile.

The ARM64 build and host CPU-helper checks pass. Run the latter with `cc -std=c11 -Wall -Wextra -Werror drivers/SwitchFan/tests/test-t214-clock.c -o /tmp/test-t214-clock && /tmp/test-t214-clock`. Signed package validation is complete. Physical CPU readback is 1996.8 MHz and the fan turns; RAM reads 1866 MHz, below the requested 2100 MHz. Windows restart remains unresolved.

## Shared battery, RTC, and volume-button support

The companion firmware's `SWITCH_LITE_SYSTEM=1` option exposes the upstream BAT0, RTC0, and BTNS definitions on Lite. It requires the Lite fan/I2C1 and power/I2C5 profiles. Use `drivers/SwitchBattery` and `drivers/SwitchRtc` directly; no Lite-specific source copies are needed.

- `switchbattery` 1.0.0.6 reads MAX17050 (I2C1, 0x36) and BQ24193 status (I2C1, 0x6B). Lite exposes exactly those two connections. The optional third BM92T connection is absent, so the USB-C role-switching timer and charger writes are not activated. The original Erista three-connection path remains available.
- `switchrtc` 1.0.0.2 reads MAX77620 RTC (I2C5, 0x68). It learns a private Windows time offset after manual/NTP time changes and applies it on later starts, preserving the hardware calendar used by Horizon.
- Volume up/down use the Windows inbox ACPI0011 button driver through the existing GPIO driver: PX6/GPIO190 and PX7/GPIO191, with both-edge interrupts. This adds input support, not an audio endpoint.

The ARM64 build, signed SYS/catalog verification, and PnPUtil staging in ARM64 WinPE 22621.1 pass. Sign the final INF/SYS catalogs and use [Install-LiteSystem.cmd](Install-LiteSystem.cmd) beside the `switchfan`, `switchpwrbtn`, `switchbattery`, `switchrtc`, and `switchaudio` package directories. Include LitePerformanceCheck.exe for clock readings and brightness controls. Run it with `online` from an elevated Home prompt, or `offline W:\` from ARM64 maintenance WinPE with WINARM's actual drive letter. Existing GPIO/I²C drivers and the Stage 6 test certificate are prerequisites. Battery readings, learned RTC time across boots, and physical volume-key input remain unverified on HDH-001.

## EMC failure and fan-floor correction

The first physical performance run reported CPU 998.4 MHz, failed BPMP initialization, and a running temperature timer at 34 C / 33 C with duty zero. SwitchFan 1.0.0.4 separates CPU preparation from EMC status and keeps the minimum fan duty for a valid configured CPU request. PMIC voltage/readback, CPU clock checks, and thermal fallback remain in place. The companion firmware corrects the ATOMICS trigger/result registers and maps the Mariko BPMP mailbox carveout. Target clock rates and cooling after this correction still await hardware confirmation.

## Audio integration and display rollback

`drivers/SwitchAudio` remains the upstream PortCls/WaveRT implementation. Its cross-build uses `portcls.lib` and the upstream native-driver linker settings. The companion firmware prepares APE, I2C1, RT5639, I2S1 and PV6 headphone detection using shared R403 code. The target is stereo speaker/headphone playback and automatic jack switching; no microphone endpoint is provided. The owner confirmed audible speaker output after the later pad/1.0.0.9 update described below. Headphones and jack switching still need acceptance.

The display-only sources stay in the existing WinX tree. After the physical Lite went black during display-device restart, CPU-rendered rotation was withdrawn from the active profile. The Home installer no longer installs WinX display or LiteLandscape, and the companion firmware uses BasicDisplay in the native orientation. The proposed PE trial was cancelled. BasicDisplay does not provide GPU rendering acceleration.

## Bluetooth preparation

The Lite build now includes the existing `drivers/SwitchBluetooth` 1.0.0.13 implementation. It uses the same UART-D H4 transport, BthMini child, patchram and 3 Mbaud flow-control path as upstream. No separate Lite Bluetooth source is added. The companion firmware's `SWITCH_LITE_BLUETOOTH=1` option supplies UART-D/PH3/PH4/PH5 initialization, the PMIC 32 kHz output, and BTH0 resources.

The installer includes Bluetooth only when a signed `switchbt/switchbt.inf` is present beside it. Supply the Switch-specific patch at `Firmware/BCM4356A3.hcd`; it is copied before the driver is staged. The blob is not part of this source repository. Switchroot documents its [NX patch source](https://gitlab.com/switchroot/android/proprietary_vendor_firmware/-/blob/main/README.md). The combined BasicDisplay/audio/Bluetooth kit is built and deployed. Signed package checks and native Windows staging pass; pairing and range remain untested on Lite. Upstream reports limited radio range while the Wi-Fi-side antenna initialization is absent.

For diagnostics, use the existing `kits/Switch-Bluetooth/Bluetooth.ps1 -Mode Status`. Its `Retry` mode clears the upstream failed-boot hold and permits another probe without replacing the driver. The radio child is deliberately added after a delay, so check Settings after it enumerates.

Do not add the Erista SwitchSensor device on Lite: [Switchroot's Lite DT](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/tegra210b01-vali.dts) reserves SPI4 for SIO. Motion data and built-in controller support need that transport rather than the direct SPI4 sensor path.

## Audio 1.0.0.8 follow-up

The physical Lite reached Home with BasicDisplay. ADMA/I2S counters advanced during playback, but the owner heard no internal-speaker output and had not tested headphones. The companion firmware now applies Switchroot's NX bias/class-D initialization for Lite. The shared 1.0.0.8 driver snapshots codec power, mixers, clocks and private tuning on jack-route changes; rendering behavior and endpoint format are unchanged. The existing one-shot TestTone option is disabled on the tested Home installation; the normal Windows startup sound is the first audible check. Actual sound remains pending.

Offline Add-Driver initially staged 1.0.0.8 without replacing the existing device's 1.0.0.7 binding. Native maintenance removed the identified superseded non-boot-critical package, retained the signed 1.0.0.8 package, and marked only the audio devnode with CONFIGFLAG_REINSTALL (0x20) for the next enumeration. See the [Windows PnP reinstall flag](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/determining-whether-a-device-is-plugged-in). Driver staging does not establish successful runtime binding or sound output.

The same boot confirmed the Bluetooth patch loaded, the 3 Mbaud path and BthMini child started without transport errors. Pairing/range still need physical confirmation. CPU readback remained 1996.8 MHz, and the owner confirmed the fan turns. EMC readback was still 204 MHz before the companion RAM R2 correction.

## R2 hardware follow-up: startup sound

The next physical boot bound audio 1.0.0.8, cleared the reinstall flag and read all recorded codec registers successfully. Jack=0 selected the speakers, SPK_VOL=0x1414 was unmuted, and ADMA completed 439 periods. These observations establish driver activation and data-path activity, not audible playback.

The owner clarified that only the Windows startup sound had been checked. The offline SOFTWARE hive had `Microsoft\Windows\CurrentVersion\Authentication\LogonUI\BootAnimation\DisableStartupSound=1`, with no overriding sound policy. The normal startup sound is enabled by setting this value to 0. The optional driver TestTone remains 0. No additional driver or firmware change is made for this configuration correction. Desktop playback and headphone output remain untested.

The same boot read CPU 1996.8 MHz and RAM 1866 MHz. BPMP rounded below the 2100 MHz request, so the firmware correctly reported a target mismatch rather than claiming 2100 MHz success.

## Backlight, audio pads and paging follow-up

`SWITCH_LITE_BACKLIGHT=1` adds PWM0 (0x7000A000, four bytes) to FAN0 separately from PWM1. Shared SwitchFan 1.0.0.5 retains the fan/CPU behavior and enables its existing brightness IOCTLs and display-power callback when PWM0 is present. It preserves the firmware divider, uses the inherited Lite brightness on first start, and restores the saved user setting thereafter. LitePerformanceCheck adds touch-sized Darker/Brighter buttons. Old Lite firmware remains supported without backlight access. This does not provide GPU acceleration or software rotation.

The owner confirmed silence during Windows Settings playback after enabling startup sound. SwitchAudio 1.0.0.8 was active, the speaker route was unmuted, codec reads succeeded, and ADMA completed transfers. The new Lite firmware explicitly selects 1.8 V for AUDIO/AUDIO_HV, exits pad DPD when required, and uses the Vali DAP1 output pinmux. These settings follow Switchroot's board DT and B01 PMC implementation; they are a candidate correction, not yet proven to restore sound. SwitchAudio 1.0.0.9 adds readback of pads, clock source, digital mixer and existing PCM-buffer peak/nonzero counts at the existing start/stop snapshots. It produces no extra sound and does no additional per-position/render work. TestTone remains disabled.

Windows kept TempPageFile=1 and created no persistent pagefile despite the requested 512–2048 MB setting. Inspection of the target SDPORT binary with Microsoft public symbols found that SdhostQueryRemovability reads `_ADR`/`_RMV` on a slot child, and SdhostPdoDeviceUsageNotification rejects paging for a removable slot. Home had no slot child. The companion `SWITCH_LITE_BOOT_SD=1` profile now describes slot 0 / memory function 0 as a fixed boot device for the Windows session. The card is removable only after shutdown. This changes ACPI metadata, not the SD miniport, card partitions or 80 GiB write limits. Returned-card readback confirms a 536870912-byte pagefile.sys, the 512–2048 MB request and no TempPageFile value; crash-dump/hibernate support is not claimed.

The paired update has been deployed to the existing WINARM installation and the card was safely ejected. Native ARM64 DISM staged SwitchFan 1.0.0.5 and SwitchAudio 1.0.0.9; both existing devnodes are marked for next-boot PnP reinstallation. The performance/brightness application is on the public desktop. The 512–2048 MB pagefile request, disabled extra test tone, completed setup state and BCD were preserved. On the next physical run, the owner confirmed audible output and working brightness buttons. Returned-card readback confirms active audio 1.0.0.9 and fan 1.0.0.5, ConfigFlags=0 for both, and a 512 MiB pagefile. Headphones, jack switching and display wake remain unconfirmed. The owner also confirmed that restart and shutdown still hang; shutdown makes the screen black without actually powering off.

References: [Switchroot display/backlight wiring](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210-nx-common-disp.dtsi), [B01 pad control](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/soc/tegra/pmc.c), and [Windows SD slot ACPI objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/other-acpi-namespace-objects#sd-host-controllers-and-devices).

The shared long-term scope, including Switchroot-based Wi-Fi and Lite SIO, is tracked in the [porting roadmap](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/PORTING.md).

## Wi-Fi and power boundaries

The companion R6 firmware probe completed a physical passive scan with six
records, terminal status 0 and UEFI return Success. PCIe supply/PHY/link,
BCM4356A3 firmware startup and control/event DMA are accepted for that
pre-Windows check. The temporary check flag was removed and Home boots Windows
normally again. This repository does not yet provide a working Windows Wi-Fi
adapter; association/key management and packet/OS integration are ongoing.
See the [shared Wi-Fi implementation and evidence](https://github.com/ruje0504-2/winx/blob/hdh001/docs/hdh001-home/WIFI.md).

Windows shutdown/restart still black-screen with the fan running. A pre-Windows
shutdown using the same Power R4 BL31 succeeded, so final Windows exit and
runtime hardware state remain under investigation.

## Windows Wi-Fi packet core

The shared [SwitchWifi packet core](../../drivers/SwitchWifi/README.md) now implements msgbuf-v5 flow-ring lifecycle, Ethernet TX/RX descriptors and buffer ownership. Host sanitizer checks and ARM64 Windows object compilation pass; WiFiCx registration, Windows DMA/interrupt transport, association/keys and OS packet queues remain unfinished. This source component is not installed on WINARM.
