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
| SD / boot | NTFS bounded writes and cold reads passed on Home. Trimmed Pro reproduced 0x7B with a two-volume miniport and one-volume BasicDisplay firmware. The trimmed Pro desktop booted with matching firmware and the owner confirmed FAT32 visibility. | Reliable read/write and cold boot within the selected 80 GiB target |
| GPIO / I2C / touch | Shared controllers loaded; Lite touch clicks work | Touch press/release and three-point check, without stuck contacts |
| CPU / fan / EMC | CPU 1996.8 MHz; fan turns; RAM 1866 MHz after BPMP rounding | Readback and thermal behavior; 2100 MHz remains a separate unmet target |
| Backlight | Owner confirmed that the deployed brightness buttons work; display off/on remains untested | Brightness adjustment and display off/on; fan PWM1 stays independent |
| Audio | Owner confirmed audible output after the pad/1.0.0.9 update; headphones and jack switching remain untested | Audible stereo speakers, headphones and jack switching; no added boot test tone |
| Battery / RTC / buttons | Shared packages integrated; full function not confirmed | Battery/charging telemetry, retained RTC offset, power and volume inputs |
| Bluetooth | Patch load, 3 Mbaud transport and BthMini startup confirmed on Home. The owner confirmed working Bluetooth on trimmed Pro with 1.0.0.13. | Discovery, pairing, data exchange and usable range |
| USB | Upstream firmware paths exist; Lite Windows support unfinished | Device enumeration and keyboard/storage operation through the supported host path |
| Built-in controls / motion | Lite SIO owns SPI4; Erista direct-SPI sensor profile is unsuitable | Lite SIO transport, button/stick input and sensor reports |
| Wi-Fi | Physical UEFI scan returned six records. On trimmed Pro 0.5.2.0 enumerated WFI0, loaded the chip firmware and processed IRQ/ring completions. Station startup failed with C0200211. After the context fix, 0.5.3.0 started the Windows station and the owner saw nearby networks. The report counted 19 scans, last successful, and zero joins. | Enumerate chip, load board firmware/NVRAM, scan, associate, DHCP and sustained traffic |
| Paging | Returned card contains a 512 MiB pagefile; 2048 MB maximum retained and TempPageFile absent | Persistent pagefile creation without the startup warning |
| Display | Trimmed Pro reaches the desktop with BasicDisplay. The owner confirmed landscape. | Preserve working output; CPU-rendered rotation and its PE trial remain cancelled |
| Power lifecycle | R4 Windows shutdown/restart black-screen with the fan running; pre-Windows shutdown with the same BL31 passed. Pre-Windows restart is untested | Trace Windows late shutdown and establish repeatable power-off/restart |

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
