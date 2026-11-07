# HDH-001 driver porting goal

The goal is a usable Windows 11 ARM64 Home system on Switch Lite HDH-001:
adapt the applicable upstream drivers and add Wi-Fi using Switchroot's Linux
implementation and board descriptions as hardware references. A successful
build or package installation is not hardware acceptance.

Shared drivers remain in their upstream directories. Only SoC-dependent code
belongs under `t214`. Source, build notes and review descriptions are English.
The sole Hekate Windows entry is **Windows 11 Lite**. Existing FAT32,
Ubuntu, Android, emuMMC and WINARM data must be preserved.

## Current work and acceptance

The owner has prioritized **S4 hibernation to SD and restoration of the saved
session**. SC7 whole-SoC retained-memory sleep is deferred to the plan. Also fix
Bluetooth initialization and resume recovery, repeated display-off/on stalls,
and the performance regression when FAT32 is visible. Build, sign and install
the coherent update on the existing card, including the FAT32-visible SDR104
profile and its matching BL33. The owner has now requested the matched hidden
profile for comparison after recurring roughly one-minute stalls. Keep both
source profiles and investigate visible-FAT performance; preserve all card data.
Code completion, build/signature checks and physical acceptance are separate.
The current S4 candidate builds and passes polled identity/write-bound tests;
saved-session restoration has not passed on the Lite. On 2026-11-07,
the signed 0.4.1.0 FAT32-visible package, matching S4 BL33, Bluetooth 1.0.0.17
and the fan/input/wake updates were installed and read back from the card.
The first cold boot stopped at recovery `0xc000000f`, before logon or any
hibernation attempt. A local card replay reproduced that error; changing only
the experimental `tegra214pep` service from boot-start to disabled let the
loader reach kernel startup. The repair removes that package and separates
the PEP ACPI option from PMIC wake resources. S4 does not require the PEP.
Its development hardware ID is also separated from the existing audio ID.
The PEP removal and paired BL33 have been installed and read back; other
driver binaries and service start modes, BCD and BL31 were preserved.
The owner confirmed a normal desktop boot with that repair on 2026-11-07.
Returned-card readback confirms S4 setup completed, AC/DC button action 2 and
a 3.7 GiB full hiberfile. The battery-only attempt powered off; the first next
boot hung black with its fan running, and the second reached the desktop.
Kernel-Boot event 16 reports `0xc000007b`; operational event 158 identifies
hiber-header validation (`0x40600017`). This is a failed attempt, not S4
acceptance. The owner confirmed no charger was connected, but no evidence
establishes battery capacity or supply interruption as the cause. Battery-only
S4 must work; external power is a diagnostic comparison, not a prerequisite.
The signed hidden-FAT comparison package is 0.4.1.1, with the same SD transfer
algorithm as 0.4.1.0. The paired firmware includes an opt-in read-only hiber
header display. Both were installed and read back on 2026-11-07. Cleanup
removed the invalidated 3.7 GiB hiberfile and its pending BCD resume request;
the next logon recreates the same single file. Other driver binaries, their
service settings and BL31 were preserved. Repeated wake, Bluetooth recovery,
S4 restoration and visible-FAT performance remain unverified.

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
| SD / boot | Both partition views booted; hiding FAT32 improved responsiveness on the earlier PIO build. The 0.4.1 series adds a 512 KiB runtime DMA limit and an opt-in S4 dump path. The installed 0.4.1.1 comparison hides FAT32 while preserving SDR104 and range filtering; the visible profile remains available in source. | Diagnose the recurring roughly one-minute stalls. Keep BL33 and the miniport on the same layout. |
| GPIO / I2C / touch | Shared controllers loaded; Lite touch clicks work | Touch press/release and three-point check, without stuck contacts |
| CPU / fan / EMC | CPU 1996.8 MHz; fan turns; RAM 1866 MHz after BPMP rounding | Readback and thermal behavior; 2100 MHz remains a separate unmet target |
| Backlight / wake | Brightness and one screen-off fan cycle passed. Repeated cycles can freeze the lock screen or leave a black screen with cursor. Fan 1.0.0.7 removes synchronous registry writes from the display notification lock. | Repeated display-off/on and unlock without stalls; pending physical test. |
| Audio | Owner confirmed audible output after the pad/1.0.0.9 update; headphones and jack switching remain untested | Audible stereo speakers, headphones and jack switching; no added boot test tone |
| Battery / RTC / buttons | Before the combined update, the Lite reported battery capacity, voltage and changing power state. The RTC read succeeded and restored system time from its saved offset. The power-button driver recorded a press and a submitted HID report. | Confirm volume-button input and observe charging transitions with the new Lite PD profile. |
| Bluetooth | Discovery and shutdown passed with 1.0.0.15 once; a later partial firmware download was falsely reported as success. Candidate 1.0.0.17 fixes failure handling and retained-patch/baud checks. | Repeated discovery and sleep/resume, preserving working shutdown. |
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

1. Finish the FAT32-visible SDR104/ADMA2 and Bluetooth/display-wake update,
   preserving normal shutdown/restart and all partitions.
2. Implement the SDPORT dump/hibernate path: bounded WINARM writes, polling
   without interrupts, and no pageable allocation, registry I/O or dependency
   on a stopped I2C driver during the final hibernation write.
3. Enable full S4 only after that path builds and passes the relevant checks.
   Verify that Windows writes the hibernation image, powers off, and restores
   the same desktop/application session through `winresume.efi`.
4. Measure FAT32-visible desktop and I/O behavior. Preserve the hidden profile;
   code/build checks do not establish a measured performance improvement.
5. Deferred: complete SC7 peripheral quiescence, PMC wake routing and
   display/storage restoration. The C7/CC6 and PMIC wake work is foundational,
   not proof of whole-SoC sleep. Keep evidence in the existing power notes.

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
