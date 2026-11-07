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

| Area | Evidence / current gap | Acceptance |
|---|---|---|
| SD / boot | WINARM-only is the default. Bounded NTFS writes and cold reads passed. Both profiles booted on the same Lite. The owner reported a much smoother desktop after hiding FAT32. | Diagnose the UI lag in the optional dual-volume profile. Keep BL33 and the miniport on the same layout. |
| GPIO / I2C / touch | Shared controllers loaded; Lite touch clicks work | Touch press/release and three-point check, without stuck contacts |
| CPU / fan / EMC | CPU 1996.8 MHz; fan turns; RAM 1866 MHz after BPMP rounding | Readback and thermal behavior; 2100 MHz remains a separate unmet target |
| Backlight | Owner confirmed that the deployed brightness buttons work; display off/on remains untested | Brightness adjustment and display off/on; fan PWM1 stays independent |
| Audio | Owner confirmed audible output after the pad/1.0.0.9 update; headphones and jack switching remain untested | Audible stereo speakers, headphones and jack switching; no added boot test tone |
| Battery / RTC / buttons | Before the combined update, the Lite reported battery capacity, voltage and changing power state. The RTC read succeeded and restored system time from its saved offset. The power-button driver recorded a press and a submitted HID report. | Confirm volume-button input and observe charging transitions with the new Lite PD profile. |
| Bluetooth | Patch load, 3 Mbaud transport and BthMini startup confirmed on Home. The owner confirmed working Bluetooth on trimmed Pro with 1.0.0.13. | Discovery, pairing, data exchange and usable range |
| USB-C host | ARM64 host firmware and signed mailbox/PD packages deployed to the card. Switchroot Ubuntu enumerated both root hubs on this Lite. Windows hardware test pending. | Enumerate a peripheral through USB-C in Windows. |
| Built-in controls / motion | Windows enumerated the gamepad and mouse and the driver received SIO input on the Lite. The card has 0.2.1 with the Capture toggle. Signed 0.2.2 adds the Plus shortcut for the On-Screen Keyboard. | Try the desktop controls and keyboard on the Lite, then verify game input and motion. |
| Wi-Fi | SwitchWifi 0.5.5 automatically reconnected to WPA2-Personal, obtained a DHCP lease and default gateway, and reached the Internet on trimmed Pro. | Measure sustained traffic and group rekey. |
| Paging | Returned card contains a 512 MiB pagefile; 2048 MB maximum retained and TempPageFile absent | Persistent pagefile creation without the startup warning |
| Display output | The owner confirmed a working landscape desktop on trimmed Pro. | Preserve working landscape output. |
| 3D graphics | Windows uses CPU software rendering only. The Tegra GPU driver for Windows has not been implemented. | Run a Direct3D workload on a Tegra hardware adapter. |
| Power lifecycle | Pre-Windows power-off passed on R4. On R5, Windows shutdown still leaves the fan running with the screen black until a long power-button press. Restart uses the same firmware path and awaits a separate R5 test. | Complete normal Windows shutdown with physical power removal. [R5 investigation](power-performance.md#windows-power-off-fallback-r5) |

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

The combined build adds `SWITCH_LITE_SIO=1` for the separate Lite UART-C controller. Offline DISM staged SwitchBattery 1.0.0.7, TegraXusb 0.1.0.0 and SwitchSio 0.2.0.0 on Windows 22631.2861. Both firmware binaries, signed packages and the two USB preparation files matched on readback. Windows hardware testing is pending for input and USB-C. [USB-C kit and install](../../WindowsDrivers/drivers/TegraXusb/README.md) · [Controller mapping for Switchroot Ubuntu](../../WindowsDrivers/drivers/SwitchSensor/README.md)

## Execution order

1. Continue Wi-Fi PCIe, firmware messaging and Windows networking integration.
   Keep Bluetooth coexistence and the shared chip's power/reset state intact.
2. Trace the remaining Windows shutdown/restart failure. Pre-Windows shutdown
   has passed; do not treat that as Windows power-lifecycle acceptance.
3. Close input and existing-driver gaps using focused physical evidence.
   Speaker output and brightness buttons are already owner-confirmed.
4. Complete USB and Lite SIO/control/sensor integration according to their
   hardware prerequisites; do not load the Erista sensor profile on Lite.
5. Record physical acceptance and update the existing firmware and driver PRs.
   Keep shared sources in their upstream locations.

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
