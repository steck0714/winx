# HDH-001 Wi-Fi bring-up

Wi-Fi is part of the active porting goal. It is not yet a working Windows adapter.

## Verified source facts

The Switchroot B01 board description selects BCM4356 FullMAC on PCIe root port index 1 (`pci@2,0`), using UPHY lane `pcie-0`. Port index 0 is disabled. The old SDIO commands in WifiBtApp are explicitly unsuitable for the Switch Wi-Fi path.

The existing upstream `Drivers/WifiBtApp/WifiPcie.h` and `WifiPexStages.h` already implement draft PCIe link, endpoint BAR, backplane/chip and firmware-console stages. They do not implement message-buffer rings, scan/join, EFI networking or a Windows network adapter. Reuse this source rather than starting a parallel Lite implementation.

Switchroot's `tegra210b01_pcie_data` differs from Erista: it does not request the Erista PCIe IO-DPD and MBIST workarounds, and its lane configuration resolves to x4+x1. Power supplies are MAX77620 LDO1/SD3. Wi-Fi and Bluetooth share the chip and low-power clock. The shared PCIe entry configures only Wi-Fi pins, leaving Bluetooth/UART pinmux intact, and skips the Erista IO-DPD workaround on B01.

The SoC-specific `t214/uphy.h` helper enables B01's 102 MHz UPHY management clock through the PLLP HSIO/XUSB branch and the repurposed CAR 0x6D0 divider. It applies Switchroot's lane-0 indirect defaults and four-entry PLL CTL10 table instead of Erista's direct CAL_CTRL/DCO_CTRL values. The shared PLLE routine selects B01's spread coefficients, and lane release respects B01's six-lane layout. An already-running UPHY sequencer keeps its calibration. These are compiled source adaptations, not evidence of a working Lite PCIe link. Rail state, root-port configuration and physical link/chip-ID readback still need acceptance.

Vali's GPIO defaults hold PH0 (RF_DISABLE) low and PH1 (Wi-Fi reset/power) high while running. The B01 PCIe path now follows those levels instead of the legacy probe's unverified active-high PH0 enable assumption. PH3/PH4 Bluetooth wake/reset are not changed by PCIe bring-up.

## Firmware loading

`wifibt pfw brcmfmac4356-pcie.bin brcmfmac4356-pcie.txt` requires both files after `pex` and `pchip`. `pall` runs those stages together and leaves the Bluetooth/UART setup alone. PCIe stage failures return a nonzero shell status as well as a log message.

The NVRAM converter follows brcmfmac's printable key/value, inline-comment, NUL-separated entry, default boardrev and aligned length-token format. It accepts a final entry without a newline and rejects empty/oversized data. Switch-specific single-board files are required; router-style multi-device files with `devpath` or `pcie/` prefixes need device selection and are rejected rather than uploaded unchanged.

Files and the combined firmware/NVRAM RAM layout are checked before halting CR4. The loader checks existing firmware-word and NVRAM-token readbacks before release, reports the shared-area handshake, and keeps console reads within chip RAM and the BAR2 aperture. Host format fixtures pass with address/undefined-behavior sanitizers, and the AARCH64 command module builds. Chip firmware startup still requires physical acceptance; these checks do not establish a working Wi-Fi link.

## Milestones

1. Adapt the shared PCIe bring-up for B01 and obtain a physical link/chip-ID readback. Do not claim a connected network from an enumerated endpoint.
2. Supply the Switch-specific `brcmfmac4356-pcie.bin` and matching board NVRAM locally. Load the chip firmware, verify its shared-memory/console handshake and retain the actual failure stage.
3. Adapt brcmfmac PCIe message-buffer rings, DMA/cache handling, interrupts and control messages. Linux cfg80211 and network callbacks need Windows equivalents; a Linux module cannot be used as a Windows SYS file.
4. Add a Windows ARM64 Wi-Fi adapter interface with scan, authentication/association and packet transmit/receive. Validate DHCP, sustained traffic, disconnect/reconnect and Bluetooth coexistence.

The firmware blob, board calibration, MAC address and credentials are not committed. The active Home package does not yet enable the unverified PCIe radio path.

## Primary references

- [B01 PCIe wiring and supplies](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi)
- [Lite GPIO running and suspend levels](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-vali-gpio.dtsi)
- [Switchroot Tegra PCIe host](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/pci/host/pci-tegra.c)
- [B01 UPHY management clock and PLLE coefficients](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/clk/tegra/clk-tegra210b01.c)
- [B01 PCIe PHY defaults and calibration](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/phy/tegra/xusb-tegra210.c)
- [brcmfmac PCIe transport](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c)
- [brcmfmac firmware/NVRAM parser](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/firmware.c)
