@echo -off
# XUSB bring-up experiment (DRAFT). Run from the SD shell. The keyboard stops working after `pad` (EHCI loses the pad),
# so this runs unattended and restores the pad at the end. Look at xusb.log afterwards: the LAST line names the step that failed.
# Built into bl33_winx_xusb.bin as the shell command `xusbinit` (no .efi needed). Needs xusb.bin (NVIDIA firmware) next to this script.
echo xusb test start
xusbinit info
xusbinit clk
xusbinit info
xusbinit pad
xusbinit fw xusb.bin
xusbinit ports
xusbinit svc 20
xusbinit restore
echo xusb test finished: see xusb.log
