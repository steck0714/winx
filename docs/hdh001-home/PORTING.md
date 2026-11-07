# HDH-001 driver porting goal

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

The new **SD 0.5.0.3 + Bluetooth 1.0.0.19** candidates are now installed.
The SD change fixes a reproduced defect: private reset-all during DDR200
fallback cleared SDPORT's interrupt masks without restoring them. The focused
regression failed before the fix and passed after it in both partition views.
ARM64 builds, signatures and installed SYS/INF/CAT readbacks passed. Existing
boot/firmware files, other drivers, service start settings and Bluetooth pairing
state were retained. The owner subsequently confirmed **desktop boot and working Bluetooth**.
Actual DDR200 selection and fallback remain unverified until runtime parameters
are read. The observed boot recovery does not prove which failed operation
triggered the earlier blue screen.

FAT32 remains visible. The latest comparison found no clear responsiveness
difference between views; both profiles remain available and performance
investigation is deferred.

Retained-memory sleep and direct power-button wake development is paused.
Its experimental code is preserved **only on
[`sc7-test`](https://github.com/ruje0504-2/winx/tree/sc7-test)**, paired with the
[driver branch](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/tree/sc7-test).
The default branches exclude the experimental BL31 context patches, sleep/wake
ACPI flags, processor PEP and input runtime-idle changes. Detailed sleep work
remains in that branch's existing power notes. SC7 is not an ACPI "S7" state
and has not passed a complete physical sleep/wake cycle. S4 stays cancelled;
its ACPI object, storage path, setup and header probe remain removed.

Fan 1.0.0.6 passed one screen-off/on cycle, but repeated cycles later stalled
the lock screen or left only a moving cursor. The independent 1.0.0.7 callback
fix remains in the default source; repeated display recovery remains unverified.
Code completion, compilation/signing and hardware acceptance are separate.

| Area | Evidence / current gap | Acceptance |
|---|---|---|
| SD / boot | The previous 0.5.0.1 + Bluetooth 1.0.0.18 combination failed with INACCESSIBLE_BOOT_DEVICE. Signed visible 0.5.0.3 + Bluetooth 1.0.0.19 is now installed; BL33 and the partition layout were retained. | Desktop boot passed on 0.5.0.3. The exact earlier failing operation was not captured. |
| SD DDR200 | Signed visible 0.5.0.3 is installed and readback-verified. It retains automatic detection, eight-tap-minimum tuning, SDR104 fallback and corrected clock ordering; it restores the event masks lost during private fallback reset. | Desktop boot passed on the replacement. Actual DDR200 activation and physical fallback remain unverified; sustained throughput is unmeasured. |
| GPIO / I2C / touch | Shared controllers loaded and Lite touch clicks work. The default branch retains the pre-sleep GPIO/I2C/touch implementations; experimental context restoration is isolated on sc7-test. | Touch press/release and three-point check without stuck contacts. |
| CPU / fan / EMC | CPU 1996.8 MHz; fan turns; RAM 1866 MHz after BPMP rounding | Readback and thermal behavior; 2100 MHz remains a separate unmet target |
| Backlight / wake | Brightness and one screen-off fan cycle passed. Repeated cycles can freeze the lock screen or leave a black screen with cursor. Fan 1.0.0.7 removes synchronous registry writes from the display notification lock. | Repeated display-off/on and unlock without stalls; pending physical test. |
| Audio | Owner confirmed audible output after the pad/1.0.0.9 update; headphones and jack switching remain untested | Audible stereo speakers, headphones and jack switching; no added boot test tone |
| Battery / RTC / buttons | Before the combined update, the Lite reported battery capacity, voltage and changing power state. The RTC read succeeded and restored system time from its saved offset. The power-button driver recorded a press and a submitted HID report. | Confirm volume-button input and observe charging transitions with the new Lite PD profile. |
| Bluetooth | Installed 1.0.0.19 retains the bounded PH4 reset/full-probe retry and prepares UART-D before HCI access. Host tests, signatures and readback pass; the owner confirmed Bluetooth working. | Bluetooth works in the observed boot; verify repeated startup and sleep/resume separately. |
| USB-C host | ARM64 host firmware and signed mailbox/PD packages deployed to the card. Switchroot Ubuntu enumerated both root hubs on this Lite. Windows hardware test pending. | Enumerate a peripheral through USB-C in Windows. |
| Built-in controls / motion | Windows enumerated the HID gamepad and mouse and received SIO input. Version 0.2.3 provides Capture, Plus, right-stick scrolling, middle click and D-pad keyboard arrows. The default branch retains 0.2.3; the display-driven 0.2.4 experiment is isolated on sc7-test. | Confirm desktop navigation, in-game controls and motion on the Lite. [Mapping](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchSensor/README.md) |
| Wi-Fi | SwitchWifi 0.5.5 automatically reconnected to WPA2-Personal, obtained a DHCP lease and default gateway, and reached the Internet on trimmed Pro. | Measure sustained traffic and group rekey. |
| Paging | Returned card contains a 512 MiB pagefile; 2048 MB maximum retained and TempPageFile absent | Persistent pagefile creation without the startup warning |
| Display output | The owner confirmed a working landscape desktop on trimmed Pro. | Preserve working landscape output. |
| 3D graphics | Windows uses CPU software rendering. The GM20B Vulkan driver is in early bring-up. | Verify GPU readback, Vulkan drawing and window presentation in order. [GPU driver status](../gpu-vulkan/STATUS.md) |
| Power lifecycle | The owner confirmed normal shutdown with complete power removal after SwitchBattery 1.0.0.8, with Wi-Fi, Bluetooth, audio and gamepad enabled. Native restart also passed on the normal R6 firmware without fan pulses. | Shutdown and native restart passed on the tested Lite. [Power status](power-performance.md) |

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

1. Desktop boot and Bluetooth now pass on SD 0.5.0.3 + Bluetooth 1.0.0.19.
   Preserve that combination, FAT32 visibility and all physical partitions.
2. Capture actual SD timing, tuning and recovery results before declaring
   DDR200 activation or SDR104 fallback accepted.
3. SC7 work is paused on the paired `sc7-test` branches. Resume only when
   requested. Keep
   its platform and input-power experiments out of normal builds and updates.
4. Keep visible-FAT performance investigation deferred; retain both profiles.

## Windows DDR200 goal

Reference Switchroot L4T Linux kernel commit
`2d0059fd3167a8df756de2aa0489d4aa70a9fc15`, especially
[SD mode selection and fallback](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/core/sd.c#L519)
and [Tegra DDR200 manual tuning](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/host/sdhci-tegra.c#L1693).

Candidate **0.5.0.3** is installed with `NV_T214_DDR200=1`. Completed SCR/CMD6
reads feed the SD 6.x/vendor-function-14 gate. It verifies SDR104, switches
the card's command system and host DDR signaling, tunes 128 taps at a
399.36 MHz module / 199.68 MHz card clock, and compares four WINARM boot-sector
readbacks before normal I/O. Failed setup triggers one bounded SDR104
reinitialization and identity check. A changed CID/RCA blocks ordinary I/O
until fresh SDPORT enumeration; physical recovery behavior still needs testing.
Host fault tests cover both partition views, narrow windows, timeouts,
corrupted readbacks and clock limits. ARM64 builds and signed package checks
pass. The earlier 0.5.0.1 combination failed with INACCESSIBLE_BOOT_DEVICE;
0.5.0.3 restores the event masks after private recovery. Its physical boot
result passed with working Bluetooth. This is not proof of active DDR200
or throughput acceptance.
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
