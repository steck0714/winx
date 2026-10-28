@echo -off
# BCM4356 Bluetooth/Wi-Fi bring-up (DRAFT, bl33_winx_wifibt.bin). Run from the SD shell; log: wifibt.log (last line = failing step).
# Optional files next to this script: BCM4356A2.hcd (BT patchram), brcmfmac4356-sdio.bin + brcmfmac4356-sdio.txt (Wi-Fi firmware+nvram).
echo wifibt test start
wifibt info
wifibt pwr
wifibt bt
wifibt bt flow=1
if exist BCM4356A2.hcd then
  wifibt bt BCM4356A2.hcd baud=3000000
endif
wifibt wifi
if exist brcmfmac4356-sdio.bin then
  wifibt wfw brcmfmac4356-sdio.bin brcmfmac4356-sdio.txt
endif
wifibt off
echo wifibt test finished: see wifibt.log
