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
and retain both storage profiles. Build, sign and install the coherent update
on the existing card, with FAT32 visible and its matching BL33. The latest
owner comparison found no clear
responsiveness difference with FAT32 visible or hidden; keep FAT32 visible.
Performance investigation is deferred, without claiming a proven cause or fix.
The owner requests the **latest Bluetooth fix and DDR200 in one card update**;
do not ship another standalone Bluetooth rollback. Preserve all card data.
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
DDR200 0.5.0.1 candidate retains the FAT32-visible header and existing BL33.
The attempted Bluetooth 1.0.0.15
rollback did not restore reliable discovery: the next record showed loaded
version 16, BootPending=1 and SafeMode=1. Candidate 1.0.0.18 is based on the
latest source, removes the persistent failed-boot hold and adds one bounded
T214 hardware-reset recovery. Both replacements passed ARM64 builds, host
fault tests and signed catalog checks. On 2026-11-07 the combined update was
installed and SYS/INF/CAT readbacks matched. Superseded Bluetooth packages
were removed; radio identity, paired-device state, other drivers and boot
configuration were preserved. Physical DDR200 mode/fallback and Bluetooth
discovery remain unverified on this replacement.
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

SC7 development now includes opt-in BL31 patch 0005 for BPMP acknowledgement
and active-low PMC wake event 51. ARM64/default builds and fault tests pass.
GPIO 1.0.0.5 saves/restores controller state without registry I/O during power
callbacks; its signed ARM64 package and repeated register-loss test pass.
Neither is installed. Complete Windows coordination and display/controller restore
before enabling whole-system sleep. [Sleep implementation](power-performance.md#current-sleepwake-work)
Opt-in BL31 patch 0006 adds shared CAR/PLL context restoration at the actual
SoC sleep/wake boundary. Normal and SC7 ARM64 builds and repeated-loss/fault
tests pass; the normal binary is unchanged by this patch. Not installed.
Pinmux, PHYs, display/device context and Windows coordination remain open.
The signed power-button 1.0.0.3 candidate adds display-aware HID wake reports,
press-only PMIC interrupts and deferred IRQ diagnostics. Callback tests pass;
it remains uninstalled and needs physical repeated-wake acceptance.
The signed I2C 1.0.0.6 candidate restores per-controller clock/reset and bus
configuration, removes D0 self-test traffic and moves error diagnostics out
of the SD/PMIC gate. Six-route context-loss/failure tests pass; not installed.
Bluetooth 1.0.0.19 is the latest signed candidate. It restores UART-D's own
clock/reset before a resumed HCI probe, preserving the 1.0.0.18 recovery path.
Repeated context-loss and failure tests pass; 1.0.0.18 is still installed.
SD 0.5.0.2 restores the saved SDR104/DDR200 source before the first card clock
pulse, and restores SDMMC1's CAR enable/reset before accessing a lost host.
Both profile builds and context-loss/failure tests pass; 0.5.0.1 stays installed.

| Area | Evidence / current gap | Acceptance |
|---|---|---|
| SD / boot | The latest owner comparison found similar responsiveness with FAT32 visible or hidden. The card now has the visible 0.5.0.1 candidate and the same paired BL33. | No proven performance difference, cause or fix. Further investigation deferred; both profiles remain available. |
| SD DDR200 | Signed visible 0.5.0.1 is installed and readback-verified: automatic detection, eight-tap-minimum tuning and SDR104 fallback. Candidate 0.5.0.2 corrects retained-power resume clock ordering; both profile builds and fault tests pass, not installed. | Physical activation, read integrity and fallback remain pending; sustained throughput is unmeasured. Sleep/wake acceptance depends on later SC7 work. |
| GPIO / I2C / touch | Shared controllers loaded; Lite touch clicks work. GPIO 1.0.0.5 and I2C 1.0.0.6 add power-transition context restoration; built, signed and host-tested, not installed. | Touch press/release and three-point check, without stuck contacts; controller restoration needs physical acceptance. |
| CPU / fan / EMC | CPU 1996.8 MHz; fan turns; RAM 1866 MHz after BPMP rounding | Readback and thermal behavior; 2100 MHz remains a separate unmet target |
| Backlight / wake | Brightness and one screen-off fan cycle passed. Repeated cycles can freeze the lock screen or leave a black screen with cursor. Fan 1.0.0.7 removes synchronous registry writes from the display notification lock. | Repeated display-off/on and unlock without stalls; pending physical test. |
| Audio | Owner confirmed audible output after the pad/1.0.0.9 update; headphones and jack switching remain untested | Audible stereo speakers, headphones and jack switching; no added boot test tone |
| Battery / RTC / buttons | Before the combined update, the Lite reported battery capacity, voltage and changing power state. The RTC read succeeded and restored system time from its saved offset. The power-button driver recorded a press and a submitted HID report. | Confirm volume-button input and observe charging transitions with the new Lite PD profile. |
| Bluetooth | Installed 1.0.0.18 removes the sticky BootPending gate and adds one T214 PH4 reset/full-probe retry. Latest signed candidate 1.0.0.19 also restores UART-D clock/reset after D0 context loss; host tests pass, not installed. | Verify discovery, repeated startup and shutdown on the replacement; complete sleep/resume acceptance separately. |
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

1. Verify the installed Bluetooth 1.0.0.18 and DDR200 0.5.0.1 combination on
   the Lite, keeping FAT32 visible. Do not deploy another separate rollback.
2. Capture actual SD timing, tuning and recovery results from the bounded
   driver diagnostics; resolve failures before claiming DDR200 acceptance.
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

Candidate **0.5.0.1** is installed with `NV_T214_DDR200=1`. Completed SCR/CMD6
reads feed the SD 6.x/vendor-function-14 gate. It verifies SDR104, switches
the card's command system and host DDR signaling, tunes 128 taps at a
399.36 MHz module / 199.68 MHz card clock, and compares four WINARM boot-sector
readbacks before normal I/O. Failed setup triggers one bounded SDR104
reinitialization and identity check. A changed CID/RCA blocks ordinary I/O
until fresh SDPORT enumeration; physical recovery behavior still needs testing.
Host fault tests cover both partition views, narrow windows, timeouts,
corrupted readbacks and clock limits. ARM64 builds and signed package checks
pass. Installation is not physical DDR200 or throughput acceptance.
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
