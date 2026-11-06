# HDH-001 Wi-Fi bring-up

Wi-Fi is part of the active porting goal. It is not yet a working Windows adapter.

## Verified source facts

The Switchroot B01 board description selects BCM4356 FullMAC on PCIe root port index 1 (`pci@2,0`), using UPHY lane `pcie-0`. Port index 0 is disabled. The old SDIO commands in WifiBtApp are explicitly unsuitable for the Switch Wi-Fi path.

The existing upstream `Drivers/WifiBtApp/WifiPcie.h` and `WifiPexStages.h` already implement draft PCIe link, endpoint BAR, backplane/chip and firmware-console stages. They do not implement message-buffer rings, scan/join, EFI networking or a Windows network adapter. Reuse this source rather than starting a parallel Lite implementation.

Switchroot's `tegra210b01_pcie_data` differs from Erista: it does not request the Erista PCIe IO-DPD and MBIST workarounds, and its lane configuration resolves to x4+x1. Power supplies are MAX77620 LDO1/SD3. UPHY and the root-port configuration must be checked against B01 before the draft Erista sequence is run on Lite. Wi-Fi and Bluetooth share the chip and low-power clock. The shared PCIe entry now configures only Wi-Fi pins, leaving the Bluetooth/UART pinmux intact, and skips the Erista IO-DPD workaround on B01. These are source adaptations, not evidence of a working Lite PCIe link; the remaining PHY/clock sequence still needs B01 review and hardware acceptance.

## Milestones

1. Adapt the shared PCIe bring-up for B01 and obtain a physical link/chip-ID readback. Do not claim a connected network from an enumerated endpoint.
2. Supply the Switch-specific `brcmfmac4356-pcie.bin` and matching board NVRAM locally. Load the chip firmware, verify its shared-memory/console handshake and retain the actual failure stage.
3. Adapt brcmfmac PCIe message-buffer rings, DMA/cache handling, interrupts and control messages. Linux cfg80211 and network callbacks need Windows equivalents; a Linux module cannot be used as a Windows SYS file.
4. Add a Windows ARM64 Wi-Fi adapter interface with scan, authentication/association and packet transmit/receive. Validate DHCP, sustained traffic, disconnect/reconnect and Bluetooth coexistence.

The firmware blob, board calibration, MAC address and credentials are not committed. The active Home package does not yet enable the unverified PCIe radio path.

## Primary references

- [B01 PCIe wiring and supplies](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi)
- [Switchroot Tegra PCIe host](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/pci/host/pci-tegra.c)
- [brcmfmac PCIe transport](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c)
- [brcmfmac firmware/NVRAM parser](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/firmware.c)
