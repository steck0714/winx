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

The owner now prioritizes **Windows DDR200 with automatic detection and SDR104
fallback**, after the requested hidden/visible-FAT responsiveness comparison.
Retained-memory sleep and direct power-button wake without Hekate remain in
the long-term goal. Implement Tegra SC7 through the
appropriate Windows power-management path; SC7 is not an ACPI "S7" state.
S4 hibernation has been cancelled and its implementation removed. Also fix
Bluetooth initialization and resume recovery, repeated display-off/on stalls,
and the performance regression when FAT32 is visible. Build, sign and install
the coherent update on the existing card, including the FAT32-visible SDR104
profile and its matching BL33. The owner reported no recurring stalls with
hidden-FAT 0.4.2.0 and reproduced a roughly one-minute stall with visible 0.4.2.1.
The latest request selects FAT32 visible again and restores Bluetooth to the
previously accepted 1.0.0.15 package. Performance investigation is deferred on
request; keep both source profiles and preserve all card data.
The long-term SD goal also includes **Windows DDR200**, porting Switchroot L4T
Linux's automatic card detection, tuning checks and SDR104 fallback below.
Code completion, build/signature checks and physical acceptance are separate.
The 0.4.2 series removes the retired T214 dump/hibernate path while retaining the
512 KiB runtime DMA limit and both partition views. The matching BL33 removes
the S4 ACPI object and temporary hiber-header screen. ARM64 builds, signed
catalog membership and both partition-profile host checks pass. The package
and BL33 were installed and read back on 2026-11-07. Hibernation, its RunOnce
setup and pending resume request were removed; the 3.7 GiB hiberfile was deleted.
The power button now requests Sleep (action 1). This policy change is not SC7
acceptance; the complete direct-wake path remains unfinished. The installed
0.4.2.1 package and paired BL33 expose FAT32. Bluetooth 1.0.0.17 was removed
from the card and the exact signed 1.0.0.15 baseline restored, with BootPending
cleared. Other drivers, service start settings, BCD and BL31 were preserved
and read back. The latest combined boot and Bluetooth discovery await a test.
The experimental processor PEP previously prevented cold boot with
`0xc000000f`; disabling it allowed the owner to reach the desktop. Its loader
failure must be resolved before using it for retained-memory sleep. The PMIC
wake resources remain independent of that backend. SC7 direct sleep/wake,
repeated display restoration and Bluetooth recovery are not implemented and
accepted as a complete system yet.

On 2026-11-07, fan 1.0.0.6 passed one display-off/on cycle, but the owner then
reported lock-screen/black-screen stalls after repeated cycles. Windows recorded
Modern Standby transitions with no DRIPS residency. The returned Bluetooth
record has SafeMode=0 and BootPending=0; patch download stopped at record 150,
yet 1.0.0.15 incorrectly published a usable radio with no HCI event replies.
These regressions remain open until the replacement is tested on the Lite.

The candidate now includes runtime display-idle policies for SIO 0.2.4 and
touch 1.0.1.1. They stop their polling through existing KMDF D0 callbacks and
request asynchronous D0 on display-on. The PMIC button provides wake input.
The firmware and remaining driver packages build; native catalog/signature
checks and focused host tests are separate from physical acceptance.

| Area | Evidence / current gap | Acceptance |
|---|---|---|
| SD / boot | Hidden-FAT 0.4.2.0 avoided the reported stalls; visible 0.4.2.1 reproduced a roughly one-minute stall. FAT32 is visible again at the owner's request. | Cause unproven; performance investigation deferred. Both profiles retain the same SDR104/range-filtering/512 KiB transfer code. |
| SD DDR200 | Capability observation, eight-tap window selection and clock-divider policy compile and pass host tests. Vendor-mode switching, physical tuning and SDR104 recovery remain unfinished. Installed packages use SDR104. | Supported cards enter DDR200; unsupported cards or failed initialization/tuning recover to SDR104 automatically. Verify actual timing, integrity, fallback and sleep/wake. |
| GPIO / I2C / touch | Shared controllers loaded; Lite touch clicks work | Touch press/release and three-point check, without stuck contacts |
| CPU / fan / EMC | CPU 1996.8 MHz; fan turns; RAM 1866 MHz after BPMP rounding | Readback and thermal behavior; 2100 MHz remains a separate unmet target |
| Backlight / wake | Brightness and one screen-off fan cycle passed. Repeated cycles can freeze the lock screen or leave a black screen with cursor. Fan 1.0.0.7 removes synchronous registry writes from the display notification lock. | Repeated display-off/on and unlock without stalls; pending physical test. |
| Audio | Owner confirmed audible output after the pad/1.0.0.9 update; headphones and jack switching remain untested | Audible stereo speakers, headphones and jack switching; no added boot test tone |
| Battery / RTC / buttons | Before the combined update, the Lite reported battery capacity, voltage and changing power state. The RTC read succeeded and restored system time from its saved offset. The power-button driver recorded a press and a submitted HID report. | Confirm volume-button input and observe charging transitions with the new Lite PD profile. |
| Bluetooth | 1.0.0.17 reported no HCI_Reset reply during the visible-FAT trial, with BootPending=0/SafeMode=0; it initialized on the next hidden trial. The card now has the owner-accepted 1.0.0.15 baseline. Its known partial-download recovery gap remains open. | Confirm restored discovery, then implement reliable repeated initialization/resume while preserving working shutdown. |
| USB-C host | ARM64 host firmware and signed mailbox/PD packages deployed to the card. Switchroot Ubuntu enumerated both root hubs on this Lite. Windows hardware test pending. | Enumerate a peripheral through USB-C in Windows. |
| Built-in controls / motion | Windows enumerated the HID gamepad and mouse and received SIO input. Version 0.2.3 provides Capture, Plus, right-stick scrolling, middle click and D-pad keyboard arrows. Candidate 0.2.4 retains those mappings and adds display-driven runtime idle. | Confirm desktop navigation, in-game controls and motion on the Lite. [Mapping](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchSensor/README.md) |
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

1. Complete the requested FAT32-visible and Bluetooth baseline restoration;
   retain SDR104 while DDR200 development continues.
2. Implement Windows DDR200 with the detection, tuning and fallback contract
   below, then build, sign, install and verify it on the Lite.
3. Complete genuine retained-memory sleep: short power-button press sleeps;
   the next press restores the same session directly, without Hekate. Complete
   the Windows power integration, SC7 firmware/context preservation and PMC
   wake routing. Do not treat screen-off, CPU idle or power-off as SC7 acceptance.
4. Complete peripheral quiescence and display/storage/Bluetooth restoration,
   preserving working shutdown/restart; verify repeated sleep and direct wake.
5. Keep visible-FAT performance investigation deferred at the owner's request;
   retain both partition profiles.

## Windows DDR200 goal

Reference Switchroot L4T Linux kernel commit
`2d0059fd3167a8df756de2aa0489d4aa70a9fc15`, especially
[SD mode selection and fallback](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/core/sd.c#L519)
and [Tegra DDR200 manual tuning](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/host/sdhci-tegra.c#L1693).

Current development build: completed SCR/CMD6 metadata reads feed the SD 6.x
and vendor-function-14 capability gate. Host tests cover the eight-tap boundary,
nonwrapping windows and 399.36 MHz host-clock divider calculation; ARM64 builds
pass for both partition profiles. This option is not installed or enabled in
the normal package. Actual mode switching, tuning and fallback are next.

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
