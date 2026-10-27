@echo -off
# BCM4356 Wi-Fi (PCIe) + Bluetooth bring-up (DRAFT). Run from the SD shell; read wifibt.log afterwards: the LAST line names the step that failed.
# Needs next to this script: brcmfmac4356-pcie.bin, brcmfmac4356-pcie.txt (Wi-Fi), BCM4356A2.hcd (Bluetooth, optional).
echo wifibt test start
wifibt info
wifibt pwr
echo --- Wi-Fi over PCIe
wifibt pex
wifibt pchip
wifibt pfw brcmfmac4356-pcie.bin brcmfmac4356-pcie.txt
wifibt pcon
wifibt poff
echo --- Bluetooth over UART-D
wifibt bt BCM4356A2.hcd
echo put a Joy-Con/Pro Controller in SYNC (pairing) mode now
wifibt btscan 15
wifibt off
echo wifibt test finished: see wifibt.log
