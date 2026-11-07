# Switch Lite USB-C host

**Windows hardware test pending.** The signed mailbox and Type-C power packages were staged on the 23H2 card with the combined BL33 and Ubuntu B01 Falcon image. Switchroot Ubuntu on the same HDH-001 starts both USB root hubs with B01 Falcon firmware 51.06. Windows peripheral enumeration, hot-plug and charging transitions still need physical evidence.

The inbox USBXHCI driver owns the host registers and SPI 39. `tegraxusb` (ACPI `SWOA0D02`) handles Falcon mailbox traffic on SPI 40 and controls LFPS on USB3 lane 5. `switchbattery` 1.0.0.7 uses the BM92T and BQ24193 for port power on `SWOA0314`. It checks the partner role before enabling VBUS and releases host power on suspend. The board policy uses the Vali current ceilings and reserves power for the port. [Switchroot host driver](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/usb/host/xhci-tegra.c) · [Vali power tree](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-vali-power-tree.dtsi)

## Package and install

1. Build `tegraxusb` and `switchbattery` with the HDH-001 ARM64 build script. Put each final SYS, its INF and its CDF together in a directory named after the service. For each directory use the Windows SDK tools below with the Stage 6 signing certificate. The target trusts its public certificate in Root and TrustedPublisher and uses Windows test signing.
2. Build WinX with the normal Lite system and Wi-Fi flags plus `-D SWITCH_LITE_XUSB=1`. Copy the resulting `UsbPrepare.efi` beside `Install-LiteSystem.cmd`. Supply the matching Switchroot Ubuntu image as `Firmware/tegra210b01_xusb_firmware`. The private binary stays outside Git. [Firmware build and extraction](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/PORTING.md#usb-c-host-on-hdh-001)
3. Run `Install-LiteSystem.cmd offline W:\ usb` on the assigned Windows volume or `Install-LiteSystem.cmd online usb` in Windows. Use `usb-input` to stage the signed `switchsio` package together with these drivers. Then deploy the matching card-specific BL33. Add `-D SWITCH_LITE_SIO=1` for the combined build.

```powershell
# Run inside each package directory. Set $name to its service name.
signtool sign /fd SHA256 /sha1 $thumbprint "$name.sys"
makecat /v "$name.cdf"
signtool sign /fd SHA256 /sha1 $thumbprint "$name.cat"
```

The firmware boots Windows with the xHCI and mailbox devices hidden if preparation fails. Successful preparation publishes them together and makes the PD controller available to the Lite battery driver. The regular Windows boot entry is **Windows 11 Lite**.
