@echo -off
# XUSB SuperSpeed experiment (DRAFT). Run AFTER xusb_test.nsh succeeded (USB2 path). Plug a USB3 device into the USB-C port through
# a USB3-capable adapter BEFORE starting. Keyboard stops after `pad`; the script restores the pad at the end. Read xusb.log:
# the LAST line names the failing step ("ss: TIMEOUT waiting for ..." = which UPHY/PLLE stage did not complete).
echo xusb ss test start
xusbinit info
xusbinit clk
xusbinit pad
xusbinit ss
xusbinit fw xusb.bin
xusbinit ports
xusbinit svc 30
xusbinit ports
xusbinit restore
echo xusb ss test finished: see xusb.log
