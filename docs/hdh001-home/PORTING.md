# HDH-001 driver porting goal

Current power/tool rows below were updated2026-11-15. Earlier dated driver
inventories are historical; use the [current DVFS record](power-performance.md#current-dvfs-integration--2026-11-15) for the active pairing.

The goal is a usable Windows 11 ARM64 system on Switch Lite HDH-001:
adapt the applicable upstream drivers and add Wi-Fi using Switchroot's Linux
implementation and board descriptions as hardware references. A successful
build or package installation is not hardware acceptance.

Shared drivers remain in their upstream directories. Only SoC-dependent code
belongs under `t214`. Source, build notes and review descriptions are English.
The sole Hekate Windows entry is **Windows 11 Lite**. Existing FAT32,
Ubuntu, Android, emuMMC and WINARM data must be preserved.

## Current work and acceptance

The previous **INACCESSIBLE_BOOT_DEVICE** regression followed
installing Bluetooth 1.0.0.18 with FAT32-visible DDR200 0.5.0.1. The owner
confirmed the stop code in a photograph. Offline logs stop before the update;
crash dumps/error logging were disabled, so the failing operation is not yet
known. The 0.5.0.3 update below restored desktop boot. Actual DDR200 activation
and physical fallback still need runtime evidence; the stop code alone did
not establish the failing operation.

The November 7 **SD 0.5.0.3 + Bluetooth 1.0.0.19** update restored boot.
The SD change fixes a reproduced defect: private reset-all during DDR200
fallback cleared SDPORT's interrupt masks without restoring them. The focused
regression failed before the fix and passed after it in both partition views.
ARM64 builds, signatures and installed SYS/INF/CAT readbacks passed. Existing
boot/firmware files, other drivers, service start settings and Bluetooth pairing
state were retained. The owner subsequently confirmed **desktop boot and working Bluetooth**.
Later runtime records are described below. The observed boot recovery does
not prove which failed operation triggered the earlier blue screen.

The November 12/13 follow-up found a bound SD INF still labelled **0.4.0.1**,
but a different active SYS containing DDR200 code. Both files were backed up
separately. The driver recorded `Ddr200Fallbacks=1`, `Ddr200Window=1`,
`Ddr200Status=0xc0000185` and successful SDR104 recovery. The later capability
cache was empty; that does not establish that the card never advertised DDR200.
The **0.5.1.1** update adds crash-dump takeover from SDR104/DDR200 and retains
capability, tuning sample count and timeout evidence across fallback.
The signed package passed native Inf2Cat checks and installation returned
3010 (reboot required). After the planned restart, **0.5.1.1 / oem89.inf /
Code 0** and its exact SYS hash were verified at 00:54 UTC. An 8 MiB
write/flush/readback passed. The new record confirms advertised DDR200
capability, **128 samples / 1 timeout / 1-tap window**, followed by successful
SDR104 recovery. Thus the scan was not cut short after one sample; the narrow
window's hardware/configuration cause remains unproven. No new volmgr event
was found by 00:55 UTC, but actual dump capture has not been tested.
[0.5.1.1 storage evidence](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/Tegra210Sdhc/README.md#ddr200-fallback-evidence-0511)

The current **0.5.1.2 / oem90.inf / Code 0** package adds explicit controller
readbacks at the DDR tuning command/clock boundaries and retains per-tap
passing/response bitmaps. The final 2026-11-12 **17:48 PST** boot
(2026-11-13 **01:48 UTC**) recorded passing taps **30 and 77**, all 128
buffer-ready responses, no timeout, a longest run of one, and successful
SDR104 recovery. The 8 MiB WriteThrough/flush/SHA-256 readback test passed;
ADMA errors were zero and no volmgr event appeared in the checked boot log.
The same boot verified **SwitchFan 1.0.0.13 / oem91.inf**, with default CPU
target and actual readback **1497.6 MHz** and CPU error zero. The original
1996800-kHz firmware request is retained separately as `CpuHandoffKhz`.
Partitions, RAM requests, the other fan settings and power settings were
unchanged. These checks establish normal storage operation and the new CPU
target, not active DDR200 or actual crash-dump capture.
[Current storage evidence](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/Tegra210Sdhc/README.md#0512-installation-and-runtime-result)

FAT32 remains visible. The latest comparison found no clear responsiveness
difference between views; both profiles remain available and performance
investigation is deferred.

SC7 retained-memory deep-sleep development is paused.
Its experimental code is preserved **only on
[`sc7-test`](https://github.com/ruje0504-2/winx/tree/sc7-test)**, paired with the
[driver branch](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/tree/sc7-test).
The default branches exclude the experimental BL31 context patches, sleep/wake
ACPI flags, processor PEP and input runtime-idle changes. Detailed sleep work
remains in that branch's existing power notes. SC7 is not an ACPI "S7" state
and has not passed a complete physical sleep/wake cycle. S4 stays cancelled;
its ACPI object, setup and header probe remain removed. The new SD crash-dump
path does not enable Windows hibernation or establish S4 resume support.

On 2026-11-08, the owner confirmed that **switchpwrbtn 1.0.0.4** restores the display and normal input after repeated short power-button screen-off/wake cycles. Runtime counters recorded **12 Power Down reports and 12 Wake Up reports**; Windows recorded paired Modern Standby entry/exit events and SSH remained reachable after wake.
This is the tested **S0 Modern Standby screen-off/wake** path. It is not SC7 deep sleep or S4 hibernation. Long-duration idle and the Windows sleep-menu entry path are not established by this short-button test.
A later long screen-off session on battery hung and required a forced power-off. Events record a Power Button standby exit after about 43 minutes, followed by an unexpected restart; the failing component remains unidentified.
The tested [1.0.0.4 source](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/tree/hdh001-winarm-bounded-write/drivers/SwitchPowerButton) is included in the default driver branch and the paired firmware submodule. Install the rebuilt 1.0.0.4 package to use the correction.

| Area | Evidence / current gap | Acceptance |
|---|---|---|
| SD / boot | Signed FAT32-visible **0.5.1.2 / oem90.inf / Code 0**, active SYS hash and restart were verified. Existing BL33 and partition extents were retained; Bluetooth remains 1.0.0.19. | 8 MiB write/flush/readback passed. This is storage validation, not a new GPU desktop acceptance. |
| SD DDR200 | Capability advertised; 128 samples, 0 timeouts, isolated passing taps 30 and 77, longest run 1 tap. The eight-tap gate rejected this result and recovered to **SDR104 / 199.68 MHz**. | Fallback and subsequent storage operation observed. Active DDR200, the narrow-window cause and sustained throughput remain unverified. |
| SD crash dumps | **0.5.1.2** retains bounded PIO takeover from SDR104/DDR200 and card/partition identity checks. No volmgr event was found in the final combined validation boot. | No actual crash dump has been generated; absence of initialization errors is not dump-capture acceptance. |
| GPIO / I2C / touch | Installed GPIO **1.0.0.4**, I2C **1.0.0.5**, Lite touch **1.0.1.2**. Default touch source remains 1.0.1.0; experimental context restoration is isolated on sc7-test. | Earlier touch clicks passed. The newer package inventory is not a fresh press/release or three-point test. |
| CPU / fan / EMC | Fan33/Core7 installed; seven power and three fan choices saved independently. Trained204/1331.2/1600/2500MHz; Noble ACTMON IRQ and CPU/display floors. | Code0, real IRQ,532 completed transitions and errors0 verified; owner manual testing passed. No duration-specific or exhaustive stress result claimed. [Current evidence](power-performance.md#current-dvfs-integration--2026-11-15) |
| Backlight / S0 wake | Installed switchpwrbtn1.0.0.7 polling/core-voltage implementation. Earlier short-button result belongs to1.0.0.4. | SC7 remains disabled; no new long-duration idle or deep-sleep acceptance. |
| Audio | Owner confirmed audible output after the pad/1.0.0.9 update; headphones and jack switching remain untested | Audible stereo speakers, headphones and jack switching; no added boot test tone |
| Battery / RTC / buttons | Battery1.0.0.14 and power-button1.0.0.7 installed; earlier RTC1.0.0.3 inventory retained. Fresh signed battery-current and net-watt telemetry verified. | No new RTC/button acceptance from this update; charging telemetry does not measure AC total power. |
| Charge termination | Battery1.0.0.14 retains hardware termination/safety timer;85%limit uses80–90%hysteresis, persisted independently of tray lifetime. | Full charge/recharge cycle remains unverified. [Current tools and policy](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/docs/hdh001/dvfs-tray.md) |
| Bluetooth | Installed 1.0.0.19 retains the bounded PH4 reset/full-probe retry and prepares UART-D before HCI access. Host tests, signatures and readback pass; the owner confirmed Bluetooth working. | Bluetooth works in the observed boot; verify repeated startup and sleep/resume separately. |
| USB-C host | ARM64 host firmware and signed mailbox/PD packages deployed to the card. Switchroot Ubuntu enumerated both root hubs on this Lite. Windows hardware test pending. | Enumerate a peripheral through USB-C in Windows. |
| Built-in controls / motion | Installed **SwitchSio 0.2.5.0**; default source remains **0.2.3.0**. Windows previously enumerated HID gamepad/mouse and received SIO input. Capture, Plus, scrolling, middle click and D-pad mappings are documented. | The installed version is not a new functional acceptance of each mapping, games or motion. [Mapping](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchSensor/README.md) |
| Wi-Fi | **SwitchWifi 0.5.7.0**, `oem88.inf`, Code 0, SYS SHA256 `61fa3cae…` reverified on November 13 00:25–00:26 UTC. The owner previously confirmed basic operation. No 0.5.8 package was found in DriverStore or the default source. | Sustained traffic, repeated reconnects and group rekey remain unverified. |
| Paging | Returned card contains a 512 MiB pagefile; 2048 MB maximum retained and TempPageFile absent | Persistent pagefile creation without the startup warning |
| Display output | The owner confirmed a working landscape desktop on trimmed Pro. | Preserve working landscape output. |
| 3D graphics | Native Vulkan triangle readback and Display-class Code 0 passed on the Lite with experimental builds. | Active WDDM panel presentation and DWM acceleration are in development. [GPU status](https://github.com/ruje0504-2/mesa-switch-woa/blob/main/tegra-x1-wddm/docs/STATUS.md) |
| Power lifecycle | The owner confirmed normal shutdown with complete power removal after SwitchBattery 1.0.0.8, with Wi-Fi, Bluetooth, audio and gamepad enabled. BL31 r8 returned Restart directly to the Windows entry and cold power-on opened Hekate. | Shutdown and native restart passed on the tested Lite. [Power status](power-performance.md) |

## USB-C host on HDH-001

Switchroot on this Lite loaded XUSB **51.06** (built 2019-10-17 16:01:14 UTC)
and registered both root hubs. Its BM92T on I²C1 address 0x18 reported type
0x0603 and revision 0x0D7C. USB 2.0 uses `usb2-0` with a +6 high-speed
current offset. SuperSpeed uses PCIe PHY lane 5; the BCM4356 uses lane 0.
The Lite power tree sets PD input ceilings of 2000, 1500, 1200 and 900 mA
at 5, 9, 12 and 15 V respectively. [Switchroot lane assignments](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi) · [Lite PD configuration](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-vali-power-tree.dtsi)

The Ubuntu boot partition already contains the matching B01 Falcon image.
Extract a private copy from that Ubuntu release's gzip-compressed U-Boot
initramfs; its header is 64 bytes. The extracted file on this unit is 122368
bytes, and its internal version and timestamp match the Linux boot log.

```sh
tail -c +65 "$initramfs" | gzip -dc | bsdtar -xOf - lib/firmware/tegra210b01_xusb_firmware > tegra210b01_xusb_firmware
```

The Windows image contains the inbox USBXHCI, UCX and USBHUB3 components. The optional `SWITCH_LITE_XUSB=1` build loads `UsbPrepare.efi` from WINARM after Wi-Fi prepares the shared B01 PHY. It loads the private Falcon image into reserved memory, brings up USB2 port 0 and USB3 lane 5, and exposes `PNP0D10` and the Falcon mailbox only after the controller is ready. The battery driver gains the BM92T connection on this profile. Keep the same WINARM-only card header when building BL33.

The combined build adds `SWITCH_LITE_SIO=1` for the separate Lite UART-C controller. Offline DISM staged SwitchBattery 1.0.0.7, TegraXusb 0.1.0.0 and SwitchSio 0.2.0.0 on Windows 22631.2861. Both firmware binaries, signed packages and the two USB preparation files matched on readback. Windows hardware testing is pending for input and USB-C. [USB-C kit and install](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/TegraXusb/README.md) · [Controller mapping for Switchroot Ubuntu](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchSensor/README.md)

## Execution order

1. Preserve installed SD 0.5.1.2, SwitchFan 1.0.0.13 with default CPU
   1497.6 MHz, Bluetooth 1.0.0.19, FAT32 visibility and all physical
   partitions. Its boot/readback result is separate from historical
   0.5.0.3 desktop acceptance and future GPU trials.
2. Investigate the recorded one-tap DDR200 window separately; do not weaken
   qualification or confuse SDR104 fallback with active DDR200. Validate
   actual crash capture separately from normal storage I/O.
3. SC7 work is paused on the paired `sc7-test` branches. Resume only when
   requested. Keep
   its platform and input-power experiments out of normal builds and updates.
4. Keep visible-FAT performance investigation deferred; retain both profiles.

## Windows DDR200 goal

Reference Switchroot L4T Linux kernel commit
`2d0059fd3167a8df756de2aa0489d4aa70a9fc15`, especially
[SD mode selection and fallback](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/core/sd.c#L519)
and [Tegra DDR200 manual tuning](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/host/sdhci-tegra.c#L1693).

The **0.5.1.2** package is built with `NV_T214_DDR200=1`. Completed SCR/CMD6
reads feed the SD 6.x/vendor-function-14 gate. It verifies SDR104, switches
the card's command system and host DDR signaling, tunes 128 taps at a
399.36 MHz module / 199.68 MHz card clock, and compares four WINARM boot-sector
readbacks before normal I/O. Failed setup triggers one bounded SDR104
reinitialization and identity check. A changed CID/RCA blocks ordinary I/O
until fresh SDPORT enumeration; those identity-mismatch cases are host-model
tests. Successful physical fallback and subsequent storage I/O are recorded above.
Host fault tests cover both partition views, narrow windows, timeouts,
corrupted readbacks and clock limits. ARM64 builds and signed package checks
pass. The earlier 0.5.0.1 combination failed with INACCESSIBLE_BOOT_DEVICE;
0.5.0.3 restores the event masks after private recovery. Its physical boot
result passed with working Bluetooth. This is not proof of active DDR200
or throughput acceptance. The newer 0.5.1.1 diagnostics distinguish a full
narrow scan from early tuning termination and preserve the original capability
result after recovery. Version 0.5.1.2 adds per-tap maps and posted-write
ordering tests; even its zero-timeout physical scan had only isolated passing
taps. This cannot identify the card's limitation or justify weakening the
eight-tap threshold.
[Implementation and diagnostics](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/Tegra210Sdhc/README.md#t214-ddr200-candidate-0501)

- Detect the SD specification revision and vendor command-system capability
  from SCR/CMD6 responses; do not assume support from branding or capacity.
- Port the CMD6 group-2 selection, T214 clock/pad setup and manual sampling
  window selection. Match the upstream rejection of windows below eight taps,
  including cards whose required DLL synchronization the host cannot provide.
- Unsupported cards stay on SDR104. A failed DDR200 transition or tuning must
  leave the card and host in a consistent, usable SDR104 mode through bounded
  recovery/reinitialization; changing a reported mode field is not fallback.
- Cover card reset, Windows boot and retained-memory resume. Record the actual
  selected mode and fallback reason in bounded existing diagnostics. Preserve
  partition filtering and all data; never use arbitrary sectors for write tests.
- Acceptance requires physical mode/readback evidence, data-integrity checks,
  automatic fallback and repeated sleep/wake. Documentation, code completion
  and successful builds do not establish DDR200 support or a throughput gain.

Detailed Wi-Fi dependencies are tracked in [WIFI.md](WIFI.md).

## Linux / Switchroot references

- [Lite device tree](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/tegra210b01-vali.dts)
- [B01 board wiring, PCIe and audio](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi)
- [Lite pinmux](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-vali-pinmux.dtsi)
- [RT5639/RT5640 codec](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/sound/soc/codecs/rt5640.c)
- [Tegra I2S](https://github.com/CTCaer/switch-l4t-kernel-nvidia/blob/linux-5.1.2/sound/soc/tegra-alt/tegra210_i2s_alt.c)
- [B01 PMC pads and power](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/soc/tegra/pmc.c)
- [brcmfmac PCIe transport](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c)

Linux provides register sequences, protocol behavior and board constraints.
The port must supply the corresponding Windows ACPI, KMDF/SPB, HID,
PortCls and networking interfaces. Board-specific firmware blobs and private
identifiers are supplied locally rather than committed to the source tree.
