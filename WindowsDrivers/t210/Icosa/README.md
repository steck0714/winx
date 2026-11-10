# T210 / Icosa / Erista Windows drivers

This directory contains the model-specific Windows package and ACPI identity for T210/Erista. Shared Wi-Fi packet transport lives in [`../../t214/Hoag/drivers/SwitchWifi`](../../t214/Hoag/drivers/SwitchWifi/README.md); Erista PCIe bring-up lives in `Drivers/WifiBtApp/t210/Icosa/`.

## BCM4356 Wi-Fi status

`drivers/SwitchWifi/package/switchwifi-erista.inf` binds only to `ACPI\SWOA0D03`. WinX exposes this ACPI device only in a build with `SWITCH_WIFI_ERISTA`, and only after its one-shot `WifiPrepare.efi` stage succeeds. This is deliberately separate from the T214/B01 `ACPI\SWOA0D01` package.

The T210/Erista route is **experimental and unverified on physical Erista hardware**. The firmware preparation reads MAX77620 state without changing PMIC registers, then runs the T210 PCIe/UPHY and Wi-Fi reset sequence. A successful PMIC read does not prove the board rails, radio power, firmware/NVRAM pair, interrupt, DMA, Windows adapter, WPA2, DHCP, or Internet access are correct. Keep the feature disabled until those checkpoints are verified on Icosa.

The Lite's physical Wi-Fi result belongs to T214/Hoag only. It cannot be presented as an Erista result. Switch L4T has Linux Wi-Fi/PCIe support on T210-family Switch systems, which provides implementation references but does not validate this Windows port.

## Opt-in firmware build

Set the EDK II defines `SWITCH_HEKATE`, `SWITCH_BOARD_ERISTA`, and `SWITCH_WIFI_ERISTA` together. The DSC rejects this option for other board profiles and rejects combining it with the T214-only `SWITCH_WIFI_RUNTIME` option. No T210 Wi-Fi preparation is included in the default firmware build.

Build the shared ARM64 `switchwifi.sys`, place it beside the Erista INF, create the catalog from `drivers/SwitchWifi/package/switchwifi-erista.cdf`, then sign the catalog and package. The CDF includes only the Erista INF and shared SYS. Provide the exact BCM4356 firmware and NVRAM/calibration files for the Erista board separately. Do not copy the Lite calibration file by assumption. For build inputs, firmware ownership and diagnostics, see the [shared SwitchWifi guide](../../t214/Hoag/drivers/SwitchWifi/README.md).

## Evidence and references

- [CTCaer Switch L4T kernel releases](https://github.com/CTCaer/switch-l4t-kernel-4.9/releases) document Switch T210/T210B01 Wi-Fi and PCIe work. That implementation is Linux-specific.
- [Linux BCM4356 PCIe driver](https://github.com/torvalds/linux/tree/master/drivers/net/wireless/broadcom/brcm80211/brcmfmac) is a protocol and firmware reference.
- [Microsoft WiFiCx client driver model](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/writing-a-wificx-client-driver) describes the Windows driver framework used by the shared miniport.
- The T214/Hoag physical report and its limitations are recorded in the [shared driver README](../../t214/Hoag/drivers/SwitchWifi/README.md).
