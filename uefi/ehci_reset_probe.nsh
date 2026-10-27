echo -off
echo ===== EHCI reset probe (run this LAST: it stops the firmware USB stack) =====
echo This emulates what the in-box Windows usbehci does at start: stop, HCRESET, then look at USBMODE.
echo Registers (op base 0x7D000130): USBCMD 7D000130  USBSTS 7D000134  PORTSC 7D000174  USBMODE 7D0001F8
echo.
echo --- BEFORE ---------------------------------------------------------
echo USBCMD
dmem 7D000130 4 -mmio
echo USBSTS
dmem 7D000134 4 -mmio
echo PORTSC
dmem 7D000174 4 -mmio
echo USBMODE   (little endian bytes; first byte 03 = host mode)
dmem 7D0001F8 4 -mmio
echo.
echo --- stop the controller (RS=0) --------------------------------------
mm 7D000130 00000000 -w 4 -mmio
stall 50000
echo USBSTS (HCHalted = bit 12 -> byte1 bit4, expect 10 or 90 in the second byte)
dmem 7D000134 4 -mmio
echo.
echo --- HCRESET ----------------------------------------------------------
mm 7D000130 00000002 -w 4 -mmio
stall 200000
echo.
echo --- AFTER ---------------------------------------------------------
echo USBCMD (bit1 HCRESET should have cleared itself)
dmem 7D000130 4 -mmio
echo USBMODE   (first byte: 03 = host mode kept, 00 = idle -> the in-box driver would NOT get a host controller)
dmem 7D0001F8 4 -mmio
echo PORTSC
dmem 7D000174 4 -mmio
echo.
echo ===== done. Take a photo of this screen. =====
