# Switch Lite HDH-001

The Lite build uses the existing Tegra210Gpio and Tegra210I2c drivers and the FTM4 implementation in `drivers/SwitchLiteTouch`. The latter matches ACPI SWOA0502, initializes the controller in Windows, handles quick press/release events and retries HID submission. The public desktop checker is built from `touch-check.c`. Real-hardware testing has confirmed a touch press on the Windows setup dialog. The three-circle TOUCH PASS test is pending.

The build script also produces the existing SwitchPowerButton driver for the optional Lite I2C5/PMIC firmware profile. It reads MAX77620 ONOFFSTAT and ONOFFIRQ. The tested 1.0.0.4 implementation sends HID Power Down while the display is on and Wake Up while it is off. Use SWITCH_LITE_POWER=1 and stage the matching switchpwrbtn.inf with the I2C driver. The independent 1.0.0.4 S0 correction is included in this branch; SC7 is not required.

The companion Windows firmware provides the GPIO interrupt resources, I²C3, touch power initialization and NTFS boot path. Its Hekate entry is **Windows 11 Lite**. [Firmware source](https://github.com/ruje0504-2/winx/tree/hdh001-hekate-sd)

On 2026-11-08, the owner confirmed that **switchpwrbtn 1.0.0.4** restores the display and normal input after repeated short power-button screen-off/wake cycles. Runtime counters recorded **12 Power Down reports and 12 Wake Up reports**; Windows recorded paired Modern Standby entry/exit events and SSH remained reachable after wake. This is the tested **S0 Modern Standby screen-off/wake** path. It is not SC7 deep sleep or S4 hibernation. Long-duration idle and the Windows sleep-menu entry path are not established by this short-button test. The tested [1.0.0.4 source](../../drivers/SwitchPowerButton) is included in this branch. Install the rebuilt 1.0.0.4 package to use the correction.

A later long screen-off session on battery became unresponsive and required a forced power-off. Windows recorded a Power Button standby exit after about 43 minutes, followed by an unexpected restart. The failing component is not identified; short-button success does not establish reliable long-duration standby.

### Optional S0 wake interrupt candidate

`switchpwrbtn` **1.0.0.5 / runtime 5** retains the 1.0.0.4 polling state
machine on existing firmware. The paired `vulkan-test` firmware can opt into
`SWITCH_LITE_POWER_IRQ=1` alongside `SWITCH_LITE_POWER=1`: PWRB then exposes
`_S0W=3`, PMC_CNTRL at `0x7000E400` (4 bytes), and the level-high wake interrupt
GSI 118 (B01 PMU_EXT / SPI 86). The driver sets only PMC's interrupt-polarity
bit 17 and restores it on removal. This adds no PEP, processor idle state or
SC7 entry/resume path and changes no Windows display/sleep timeout.

With those resources, a passive wake-capable WDF interrupt replaces polling.
The PMIC's ONOFF top mask, global interrupt mask and EN0 edge masks are saved;
unrelated bits are preserved, release is masked and press emits a complete
Power Down/Wake Up pulse according to display state. D0 resume leaves the
latched edge for the ISR. Failed mask restoration retains the snapshot; an
I2C fault retires the device through PnP rather than claiming wake readiness.
Registry counters run outside the ISR lock and IRQ power callbacks.

`WakeIrqMode`, `WakeIrqReady`, `S0WakePolicyStatus`, `SxWakePolicyStatus`,
`WakeIrqError` and `Irqs` use the existing service Parameters key. Mode 0 is
the legacy polling path; build/model success is not an IRQ hardware pass.
Local validation covers actual power/IRQ callbacks with explicit WDF/I2C/VHF
substitutes and mask transactions with injected read/write/readback failures:

```sh
cc -std=c11 -Wall -Wextra -Werror drivers/SwitchPowerButton/tests/test_button_state.c -o /tmp/swpwr-button
/tmp/swpwr-button
cc -std=c11 -Wall -Wextra -Werror drivers/SwitchPowerButton/tests/test_irq_route.c -o /tmp/swpwr-route
/tmp/swpwr-route
python3 drivers/SwitchPowerButton/tests/test-wake-callbacks.py
```

The candidate passes these local checks and a real-WDK ARM64 build. No physical
acceptance of the interrupt-wake path is recorded. The tested 1.0.0.4 polling
path remains the hardware baseline.

## Compile the Lite driver set

Set WDK_INCLUDE to the WDK Include/version directory, WDK_LIB to Lib/version/km/ARM64, WDF_INCLUDE to Include/wdf/kmdf/1.15, and WDF_LIB to Lib/wdf/kmdf/ARM64/1.15. CLANG_CL and LLD_LINK select the tools. WDK_EXTRA_LIB can select the matching SPB, GPIO, VHF, and battery-class (`battc.lib`) libraries. The script builds GPIO, I²C, Lite touch, power button, fan, battery, RTC, and Bluetooth.

```sh
sh docs/hdh001/build-drivers.sh . /path/to/output
python3 docs/hdh001/tests/test-ftm4.py
```

The existing `tools/Build-Driver.ps1` also accepts the Lite source via its Sources argument and VHF libraries via Libs. Sign the final SYS and catalog files with the same development certificate used by the target Windows image. Install the GPIO, I²C and touch packages into the offline ARM64 image with DISM, then place the checker at `Users/Public/Desktop/LiteTouchCheck.exe`. Add the power-button package when using the optional power-enabled firmware.

## SD system partition

The bounded-write profile is in `drivers/Tegra210Sdhc/winarm`. Use the existing target generator with a saved image to produce the private header, and build the optional WINARM profile as described in the SD README. The current INF is version 0.5.0.3. The owner confirmed desktop boot with FAT32 visible and Bluetooth 1.0.0.19; actual DDR200 mode and throughput still need runtime evidence.

The returned in-memory MBR places the configured WINARM partition in slot 1 and sets its active flag. This lets the system-partition check used by Windows Setup accept the already-booted MBR volume. The physical card's identifiers and full write bounds remain part of the selected profile's checks.

For the interrupted Home installation tested here, Windows subsequently required its failed specialize state to be restored before retry. The firmware repository records those setup logs and the recovery steps.

## Shared fan and T214 clock control

`drivers/SwitchFan` now supports both the original SWOA0401 device and Lite SWOA0402. Its common fan/I2C/backlight code stays in the upstream source file; only the Mariko clock helper and handoff definition are under `src/t214`. The original device retains its backlight interface. Older Lite firmware exposes PWM1 only; the new optional backlight profile adds a separate PWM0 resource and enables the existing brightness interface.

On Lite, TMP451 uses the Mariko -12.5 C remote offset. The optional firmware handoff requests CPU 1996.8 MHz with MAX77812 M4 at 1120 mV, independently of whether the firmware completed the requested EMC change. The fan timer owns both cooling and CPU transitions. At 80 C or after repeated sensor-read failures it restores the saved boot CPU frequency; at 70 C it can resume the request. It does not touch MAX77812 M1/GPU registers.

Use the matching firmware's `cpu_oc`/`ram_oc` INI options and its LitePerformanceCheck application. Parameters records include CpuActualKhz, RamActualKhz, TempSoC, TempBoard, Duty, SensorFailures and status. Build with `docs/hdh001/build-drivers.sh`; `vulkan-test` additionally carries the kernel-only GPU PMIC bridge. The existing Erista CPU-clock driver is not loaded for the Lite profile.

The `vulkan-test` 1.0.0.11 diagnostic also reads the existing service Parameters
DWORD `CpuOverclockEnabled` once at hardware start (default 1). Set it to 0
before restarting the device to retain the inherited CPU clock through display
changes; thermal fan control, backlight and the kernel-only GPU PMIC bridge
remain available. This permits a stock-clock hang comparison without changing
the firmware handoff or Windows display/sleep settings. It is not a confirmed
hang fix; changing the registry value alone does not change a running clock.

Version 1.0.0.6 handles Lite screen-off cooling using the existing display-power notification. It restores the saved boot CPU clock before removing the overclock fan floor. The normal temperature curve stops the fan at 39 C or below and restarts it at 42 C; hot screen-off workloads still get cooling. Failed clock restoration retains the fan floor, and a failed temperature read restarts an already stopped fan immediately. On display wake the fan is set before CPU overclock can resume. A cool system sleep/hibernate D0 exit can also stop the fan; final removal, shutdown and device-only idle retain their existing cooling behavior.

The ARM64 build, CPU/backlight checks, and `python3 drivers/SwitchFan/tests/test-screenoff.py` pass. The latter exercises the actual timer and D0 callbacks, including clock/fan ordering, invalid temperatures, wake and shutdown isolation. Native catalog generation, signing and offline installation readback passed. Fan stop/wake and brightness have physical results. On 2026-11-08, repeated short-button screen-off/wake passed after installing switchpwrbtn 1.0.0.4, with normal display and input confirmed by the owner. `ScreenOffCooling`, `SleepFanOff`, `CpuOverclockActive`, `Duty` and `DriverVersion` update fixed values under the driver's existing Parameters key; no periodic log files are created.

Screen-off cooling does not establish SoC low-power sleep. The observed Modern Standby records had no DRIPS residency, and S4 hibernation is disabled in the tested installation. Physical CPU readback on 1.0.0.5 is 1996.8 MHz and RAM is 1866 MHz, below the requested 2100 MHz. Normal shutdown and native restart have separate owner-confirmed results in the [power status](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/power-performance.md).

## Experimental sleep branch

SC7 retained-memory deep-sleep development is paused;
its experimental code is preserved only in
[`sc7-test`](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/tree/sc7-test),
paired with [WinX `sc7-test`](https://github.com/ruje0504-2/winx/tree/sc7-test).
The default branch excludes the processor PEP, PMIC wake-interrupt path,
GPIO/I2C sleep-context experiments and display-driven SIO/touch runtime idle.
The default branch retains the pre-experiment GPIO/I2C/RTC/touch/SIO and
the independently tested 1.0.0.4 S0 power-button correction. No SC7 wake
interrupt or platform sleep policy is imported.
S4 hibernation remains removed; SC7 has not passed hardware acceptance.

Independent fixes remain here: fan 1.0.0.7 keeps registry I/O outside the
backlight notification lock; Bluetooth 1.0.0.19 retains bounded chip recovery
and UART host preparation; SD 0.5.0.3 retains DDR200 development and its
retained-card-power host recovery. None enables platform sleep or depends on
the experimental PEP. Working battery/shutdown fixes are retained.

On 2026-11-07 the installed Bluetooth 1.0.0.18 + DDR200 0.5.0.1 combination
failed to reach the desktop with **INACCESSIBLE_BOOT_DEVICE**, confirmed in
the owner's photograph. The returned card had crash dumps/error logging
disabled and no new driver diagnostics; the failing operation remains unknown. No replacement was
written during the branch split. Afterwards, SD 0.5.0.3 and Bluetooth 1.0.0.19
were installed and their signed package/SYS readbacks matched. The SD update
fixes event-mask loss reproduced in the recovery callback; a successful
physical boot and Bluetooth are now owner-confirmed; actual DDR200 mode
and fallback remain unverified. See the [SD candidate status](../../drivers/Tegra210Sdhc/README.md#t214-ddr200-candidate-0501).

## Shared battery, RTC, and volume-button support

The companion firmware's `SWITCH_LITE_SYSTEM=1` option exposes the upstream BAT0, RTC0, and BTNS definitions on Lite. It requires the Lite fan/I2C1 and power/I2C5 profiles. Use `drivers/SwitchBattery` and `drivers/SwitchRtc` directly; no Lite-specific source copies are needed.

- `switchbattery` 1.0.0.11 reads MAX17050 (I2C1, 0x36) and configures BQ24193 (I2C1, 0x6B). These two connections suffice for battery reporting and charge termination. The optional third BM92T connection is exposed when the companion firmware successfully prepares the USB-C host; it enables the separate PD/role-switching path. Its absence does not disable the charging policy below.
- `switchrtc` 1.0.0.2 reads MAX77620 RTC (I2C5, 0x68). It learns a private Windows time offset after manual/NTP time changes and applies it on later starts, preserving the hardware calendar used by Horizon.
- Volume up/down use the Windows inbox ACPI0011 button driver through the existing GPIO driver: PX6/GPIO190 and PX7/GPIO191, with both-edge interrupts. This adds input support, not an audio endpoint.

Sign the final INF/SYS catalogs and use [Install-LiteSystem.cmd](Install-LiteSystem.cmd) beside the `switchfan`, `switchpwrbtn`, `switchbattery`, `switchrtc`, and `switchaudio` package directories. Include LitePerformanceCheck.exe for clock readings and brightness controls. Run it with `online` from an elevated Home prompt, or `offline W:\` from ARM64 maintenance WinPE with WINARM's actual drive letter. Existing GPIO/I²C drivers and the Stage 6 test certificate are prerequisites. Battery capacity, voltage and online/charging state have live Lite results; volume-key acceptance is separate.

### Charge termination

SwitchBattery **1.0.0.11** enables BQ24193 `EN_TERM` and the eight-hour charge safety timer at self-managed I/O startup/restart, including the two-connection profile without BM92T. The chip ends charging using its configured voltage/current thresholds and automatically starts a new charge cycle when voltage falls below the recharge threshold. Eight hours is an abnormal-charge timeout, not a delay before recharging. The driver does not periodically reset that timer or force charging from the displayed percentage.

Only REG05 is changed: voltage, fast-charge current, termination current, input limits, charge/OTG mode and temperature settings are preserved. The watchdog remains disabled. Unrecognized charger IDs, FORCE_20PCT operation, I2C failures and readback mismatch are reported rather than treated as successful configuration. Fixed `ChargeTimerBefore`, `ChargeTimerAfter` and `ChargePolicyStatus` values use the existing Parameters key; no diagnostic service or periodic log file is added.

This differs from [Switchroot's charger policy](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/power/supply/bq2419x-charger.c): Linux initially disables termination and the safety timer and provides a software full-charge termination callback. Windows uses the [BQ24193 autonomous termination and safety timer](https://www.ti.com/lit/ds/symlink/bq24193.pdf), sections 8.3.3.5–6, instead of that Linux framework.

On 2026-11-08, ARM64 compilation, native SYS/catalog signature verification and live PnP installation passed. Runtime version 11 reported successful REG05 readback **0x02 → 0x8A**, with the battery device started and charging at about 91%. This confirms configuration, not a complete full-charge/recharge cycle. Earlier 100% gauge readings with positive current did not show voltage above the configured 4.192 V limit. Run `python3 drivers/SwitchBattery/tests/test-charge-policy.py` and `python3 drivers/SwitchBattery/tests/test-irp-ownership.py` for the register/error-path and battery request-ownership tests.

## EMC failure and fan-floor correction

The first physical performance run reported CPU 998.4 MHz, failed BPMP initialization, and a running temperature timer at 34 C / 33 C with duty zero. SwitchFan 1.0.0.4 separates CPU preparation from EMC status and keeps the minimum fan duty for a valid configured CPU request. PMIC voltage/readback, CPU clock checks, and thermal fallback remain in place. The companion firmware corrects the ATOMICS trigger/result registers and maps the Mariko BPMP mailbox carveout. Target clock rates and cooling after this correction still await hardware confirmation.

## Audio integration and display rollback

`drivers/SwitchAudio` remains the upstream PortCls/WaveRT implementation. Its cross-build uses `portcls.lib` and the upstream native-driver linker settings. The companion firmware prepares APE, I2C1, RT5639, I2S1 and PV6 headphone detection using shared R403 code. The target is stereo speaker/headphone playback and automatic jack switching; no microphone endpoint is provided. The owner confirmed audible speaker output after the later pad/1.0.0.9 update described below. Headphones and jack switching still need acceptance.

The display-only sources stay in the existing WinX tree. After the physical Lite went black during display-device restart, CPU-rendered rotation was withdrawn from the active profile. The Home installer no longer installs WinX display or LiteLandscape, and the companion firmware uses BasicDisplay in the native orientation. The proposed PE trial was cancelled. BasicDisplay does not provide GPU rendering acceleration.

## Bluetooth preparation

The shared `drivers/SwitchBluetooth` implementation uses UART-D H4, the
inbox BthMini child, patchram and 3 Mbaud flow control. The companion firmware's
`SWITCH_LITE_BLUETOOTH=1` option supplies UART-D/PH3/PH4/PH5, the PMIC 32 kHz
output and BTH0 resources. Do not add a separate Lite Bluetooth source copy.

The previous **1.0.0.18** trial retains strict patch/status/baud verification
and HCI cancellation/drain. A stale `BootPending` value is diagnostic only;
it no longer disables all later boots, and orderly D0 exit clears it. On a
failed T214 probe, one PH4 reset and full retry are permitted before UART/HCI
traffic starts. PH3 is held awake; Wi-Fi power and the shared 32 kHz clock are
untouched. These follow the Switchroot B01 Bluetooth wiring and the existing
Lite DXE power sequence. ARM64 compilation, host fault tests and signed
catalog checks pass. It was installed with DDR200 0.5.0.1 on 2026-11-07;
SYS/INF/CAT readback matched. Older matching Bluetooth packages were removed
to prevent reselection, while the existing radio identity and paired-device
registry state were preserved. Repeat physical discovery and shutdown remain
unverified for this replacement.

The latest signed candidate is **1.0.0.19**. It also restores UART-D's own
clock enable/reset before probing after D0 context loss, and rejects an
unavailable PLLP parent without touching UART registers. A healthy cold-start
host is left enabled; shared PLLP, Wi-Fi power and paired-device state are not
modified. Actual-callback tests cover repeated host context loss, inaccessible
clocks, stuck reset/enable bits and orderly D0 cleanup. ARM64 compilation and
SYS/catalog verification pass. It is now installed with DDR200 0.5.0.3;
package readback, radio identity and paired-device checks passed. The owner
confirmed successful desktop boot and working Bluetooth on this combination;
repeated startup and sleep/wake remain separate tests.
[Switchroot UART initialization](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/tty/serial/serial-tegra.c#L1108)

The installer includes Bluetooth when a signed `switchbt/switchbt.inf` is
present. Supply the private Switch-specific patch at `Firmware/BCM4356A3.hcd`;
it is copied before driver staging and is not distributed in this repository.
Use `kits/Switch-Bluetooth/Bluetooth.ps1 -Mode Status` to read bounded existing
diagnostics. The child is added after a delay. Check the actually loaded
`DriverVersion`, since an older staged package can supersede a file-only rollback.

### Built-in gamepad and motion

On HDH-001 the internal SIO controller owns the IMU. The optional
`switchsio.inf` package binds to `ACPI\SWOA0902` on UART-C. It uses VHF
for 14 buttons, a hat and four stick axes, and serves the latest accelerometer
sample through the existing SwitchSensor status IOCTL. The Home button uses
its separate GPIO. Build the firmware with `-D SWITCH_LITE_SIO=1` alongside
the existing Lite touch/GPIO profile. Install the signed driver package with
`pnputil /add-driver switchsio\switchsio.inf /install`.

The protocol fixture covers the handshake, split replies, damaged headers,
stick limits, button mapping and partial motion samples. The ARM64 driver and
optional firmware build successfully. Physical input and motion are pending.
[Switchroot Lite device tree](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/tegra210b01-vali.dts)


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

The paired update has been deployed to the existing WINARM installation and the card was safely ejected. Native ARM64 DISM staged SwitchFan 1.0.0.5 and SwitchAudio 1.0.0.9; both existing devnodes are marked for next-boot PnP reinstallation. The performance/brightness application is on the public desktop. The 512–2048 MB pagefile request, disabled extra test tone, completed setup state and BCD were preserved. On the next physical run, the owner confirmed audible output and working brightness buttons. Returned-card readback confirms active audio 1.0.0.9 and fan 1.0.0.5, ConfigFlags=0 for both, and a 512 MiB pagefile. Headphones and jack switching remain unconfirmed. Later updates passed normal shutdown/native restart, and switchpwrbtn 1.0.0.4 passed repeated short-button screen-off/wake; see the current results above.

References: [Switchroot display/backlight wiring](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210-nx-common-disp.dtsi), [B01 pad control](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/soc/tegra/pmc.c), and [Windows SD slot ACPI objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/other-acpi-namespace-objects#sd-host-controllers-and-devices).

The shared long-term scope, including Switchroot-based Wi-Fi and Lite SIO, is tracked in the [porting roadmap](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/PORTING.md).

## Wi-Fi and power boundaries

**Wi-Fi works on Switch Lite (HDH-001).** SwitchWifi 0.5.5 connected automatically to a saved WPA2-Personal network on Windows 11 ARM64 23H2. Windows recorded successful authentication, an IPv4 DHCP lease and a default gateway. The owner confirmed Internet access. [Driver and hardware results](../../drivers/SwitchWifi/README.md)

Normal Windows shutdown powered off the tested HDH-001 after installing
SwitchBattery **1.0.0.8**, with Wi-Fi, Bluetooth, audio and the gamepad enabled.
The owner observed the R6 diagnostic firmware's two fan pulses followed by
complete power removal. No device-isolation utility was needed. The normal R6 firmware selects native PMC/Hekate restart and disables the
diagnostic fan pulses. The owner subsequently confirmed that Windows Restart
also completed normally.

The previous battery IOCTL handler passed the underlying IRP of a KMDF-owned
request to `BatteryClassIoctl`, which can complete or retain that IRP itself.
Version 1.0.0.8 uses a WDM preprocess callback before KMDF takes ownership, as in
[Microsoft's battery sample](https://github.com/microsoft/Windows-driver-samples/blob/main/simbatt/func/wdf.c).
A separate lock protects the battery class handle without blocking the I2C
lock used by its callbacks. Only unhandled IRPs continue into KMDF. The actual
handler's immediate, pending, failed and fallback paths passed an ASan/UBSan
host test; the ARM64 package and signatures were checked before deployment.
Run `python3 drivers/SwitchBattery/tests/test-irp-ownership.py` for that test.
The installed Bluetooth 1.0.0.15 cancellation fix is also retained; it did not
resolve the full shutdown failure on its own. [Firmware and physical evidence](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/power-performance.md)

## Windows Wi-Fi packet core

The ARM64 WiFiCx driver uses NetAdapterCx packet queues and the Broadcom msgbuf transport. Current hardware evidence and the meaning of each diagnostic field are in the [driver notes](../../drivers/SwitchWifi/README.md).


For a Wi-Fi-only kit, use `Install-LiteSystem.cmd online wifi` or
`Install-LiteSystem.cmd offline W:\ wifi`. Supply the signed `switchwifi`
directory, matching `WifiPrepare.efi` and the private A3 firmware/NVRAM
pair in `Firmware`. The installer keeps an existing complete board pair.
The runtime BL33 must be prepared separately.
