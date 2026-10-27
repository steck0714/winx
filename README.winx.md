# winx (win nx) - UEFI (EDK2) bl33 for Nintendo Switch / Tegra210, launched from hekate

A fork-style patch set on top of [imbushuo/NintendoSwitchPkg](https://github.com/imbushuo/NintendoSwitchPkg)
(commit `09c3ade`, EDK2 `edk2-stable202402`). Goal: boot Windows on ARM on a Tegra210 Switch.
**Work in progress - Windows has not been booted yet.** / 作業中。Windowsはまだ起動していません。

## Status (reported from a real Erista console)
| | |
|---|---|
| hekate -> BL31 -> bl33 -> UEFI Shell | works |
| Shell orientation (landscape) | works (`SWITCH_FB_ROTATE=2`); OS loaders get the native portrait mode |
| USB host + **USB keyboard** in the Shell | works (bm92t36 PD + bq24193 OTG handling) |
| microSD visible (`map`) after hekate | works with the SDMMC power-up patch |
| `startup.nsh` auto-run | not confirmed |
| Windows boot manager / WinPE | **not tried yet** |
| USB mass storage, PD/charging side effects | untested |

Not verified: Mariko / Lite / OLED consoles. Only the author's console was used.

## What you need to supply
* hekate (GPL-2.0), `bl31.bin` (Arm Trusted Firmware for Tegra210, e.g. from a switchroot/LineageOS package).
* This repository's `bl33.bin` (`release/bl33.bin`) or build it yourself.

## hekate setup
`bootloader/hekate_ipl.ini`
```
[Windows ARM64]
l4t=1
boot_prefixes=switchroot/winx/
id=WINXHKT
```
Put `bl31.bin` and `bl33.bin` in `switchroot/winx/`. UEFI looks for `\EFI\BOOT\BOOTAA64.EFI` on the SD card FAT volume.
If you use a UEFI Shell binary there, it stays landscape.

## Build (Linux, GCC5, `aarch64-linux-gnu-`)
```
git clone --depth 1 --branch edk2-stable202402 https://github.com/tianocore/edk2.git
(cd edk2 && git submodule update --init --depth 1 BaseTools/Source/C/BrotliCompress/brotli \
   MdeModulePkg/Library/BrotliCustomDecompressLib/brotli MdePkg/Library/MipiSysTLib/mipisyst)
# put this tree (or upstream 09c3ade + patches/winx-bl33.patch) next to edk2 as NintendoSwitchPkg
# Include/FwReleaseInfo.h: #define __IMPL_COMMIT_ID__ "xxxxxxxx" and __RELEASE_DATE__ "mm/dd/yyyy"
export WORKSPACE=$PWD/edk2 PACKAGES_PATH=$PWD/edk2:$PWD GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
cd edk2 && . edksetup.sh && make -C BaseTools
build -a AARCH64 -p NintendoSwitchPkg/NintendoSwitch.dsc -t GCC5 -b RELEASE \
      -D SWITCH_HEKATE=1 -D SWITCH_USB=1 -D SWITCH_FB_ROTATE=2
R=Build/NintendoSwitch-AARCH64/RELEASE_GCC5
python3 ../NintendoSwitchPkg/Tools/make-bl33.py --fd $R/FV/TEGRA210_EFI.fd \
        --map $R/FV/FVMAIN_COMPACT.Fv.map -o bl33.bin
```
Switches: `SWITCH_HEKATE` (hekate boot chain), `SWITCH_USB` (EHCI/UsbBus/UsbKb/UsbMassStorage),
`SWITCH_FB_ROTATE=1|2` (landscape; 2 was the right way up on the author's unit), `SWITCH_FAN_DUTY=1..235` (optional fan).

## Integrated full build
`Tools/build-full.sh` builds everything at once (all switches above, including `SWITCH_XUSB` and `SWITCH_WIFIBT`); `release/bl33.bin` is that build
(FD 95% full; append `-D SWITCH_BIGFD=1` if it stops fitting). Hardware test scripts for the SD card are in `tests/` (`winx_test.nsh`, `xusb_test.nsh`, `wifi_test.nsh`);
each experiment writes a log file (`xusb.log`, `wifibt.log`) whose LAST line names the failing step. Everything marked DRAFT is unverified on hardware.

## Known caveats
* Windows on a Cortex-A57 (ARMv8.0): recent Windows 11 builds need ARMv8.1 atomics, so use Windows 10 ARM64
  or an older Windows 11 build (about 23H2 and earlier).
* The ACPI tables only describe CPU/GIC/timer/UART at the moment; Windows has no SD, USB, GPU, input drivers yet.
* After `ExitBootServices` the firmware USB/PD handling stops.

## Optional feature switches (all UNTESTED on hardware until verified, see docs/ in the release notes)
Pass as extra `-D NAME=1` to `build` (together with `SWITCH_HEKATE=1 SWITCH_USB=1 SWITCH_FB_ROTATE=2`). The FD stays at
0xE0000 (bl33.bin = 921600 bytes) for every combination below; `SWITCH_BIGFD=1` grows it to 0xFF000 if it ever stops fitting.

| Switch | Adds | Notes |
|---|---|---|
| (always) | `acpiview` shell command | `UefiShellAcpiViewCommandLib` linked into the Shell |
| `SWITCH_EXT` | USB mouse (`UsbMouseDxe`) + `MouseCursorDxe` (software arrow cursor on the GOP with acceleration, red/blue while left/right button is held, hidden until the first mouse event; the SimplePointer events are shared with the Shell editors/HII, not stolen), `RamDiskDxe` | RAM disks are created from the HII form (UiApp > Device Manager > RAM Disk Configuration); EDK2's shell has no `ramdisk` command |
| `SWITCH_RTC` | real `date` / `time` | MAX77620 PMIC RTC (I2C 0x68 on I2C5), read once at boot, then a soft clock; `SetTime` writes the RTC before ExitBootServices. Falls back to build date + uptime if the RTC is unreadable. RTC holds UTC (TimeZone 0) |
| `SWITCH_NET` | UEFI IPv4 stack (Mnp/Arp/Ip4/Udp4/Dhcp4/Mtftp4), USB NIC drivers CDC-ECM, CDC-NCM, RNDIS, `tftp` | `ifconfig` / `ping` already live in the Shell (Network1 lib); they need a NIC to be useful |
| `SWITCH_XUSB` | `xusbinit` | see below |
| `SWITCH_WIFIBT` | `wifibt` | see below |
| `SWITCH_EXCFB` | exception dump on the panel | An unexpected CPU exception (e.g. an alignment fault, `ESR 0x96000061`) draws PC/ESR/FAR/X0-X30/SP + stack dump on the screen (white on blue, landscape if `SWITCH_FB_ROTATE` is set) and then halts. Photograph it and send it back. Replaces `DefaultExceptionHandlerLib` with `Library/FbExceptionHandlerLib`; works in RELEASE builds. Host test of the renderer: `Tools/host-tests/fbtext` |
| `SWITCH_ACPI_XHCI` | XUSB xHCI in the DSDT | Adds `XHC0` (`NVDA0210`, compatible ID `PNP0D10`, 0x70090000/0x8000, INTID 71) to the DSDT. Only meaningful after `xusb.bin` is loaded and the mailbox is serviced; off by default. `SWITCH_ACPI_EMMC` (eMMC visible to Windows) is the other ACPI switch. |
| `SWITCH_PERF` | `dp` + core performance log | Changes DxeCore (DxeCorePerformanceLib). Most intrusive switch |

Not implemented (needs hardware work that cannot be verified remotely): USB 3.0 (Tegra210 XUSB needs the xHCI firmware blob and
XUSB padctl bring-up), a real Bluetooth host stack / 802.11 network driver (only the `wifibt` bring-up experiment below exists), USB NIC chipsets that are not CDC/RNDIS (e.g. AX88179).

## XUSB (USB 3.0) bring-up experiment: `SWITCH_XUSB` (DRAFT, UNTESTED)
`-D SWITCH_XUSB=1` additionally builds `XusbInit.efi` (Shell application) and links the same code into the FD as the built-in shell command `xusbinit`. It is a step-by-step Tegra210 XUSB
host bring-up based on Linux `xhci-tegra.c` / `xusb-tegra210.c` and hekate `xusbd.c`: `info`, `clk`, `pad`, `ss`, `fw [xusb.bin]`, `svc`, `ports`,
`restore` (plus `pmc` / `pg`, which touch PMC and may fault from non-secure). Every line is written to `xusb.log` and flushed, so the last
line names the step that faulted (alignment faults: look at the faulting address in the exception dump and the last log line).
`xusb.bin` is NVIDIA's firmware (Linux `nvidia/tegra210/xusb.bin`), not included. `ss` (DRAFT) brings up SuperSpeed: PLLE, the UPHY PLL, lane `pcie-6` (usb3-ss, USB3 port 0 with companion usb2-0) and the SS port; run it after `clk` and `pad`
(`all` does). `svc` answers the firmware mailbox like Linux xhci-tegra (falcon/SS clock, LFPS detect enable/disable, SAVE_DFE_CTLE_CTX -> NAK). Windows has no driver
that answers this mailbox, so the xHCI is only usable while UEFI is running. `pad` takes the USB-C USB2 pad away from EHCI, so the firmware keyboard stops until `restore`.

## Bluetooth / Wi-Fi bring-up experiment: `SWITCH_WIFIBT` (DRAFT, UNTESTED)
`-D SWITCH_WIFIBT=1` links the built-in shell command `wifibt` for the Switch's single Broadcom BCM4356 combo chip. It is a diagnostic
bring-up tool, NOT a network/Bluetooth driver: EDK2 has no Bluetooth stack and no 802.11 stack that fits here, so nothing is registered with
UEFI protocols. Every step logs + flushes to `wifibt.log`; the LAST line names the step that failed. The module is built with `-mstrict-align`
and the plain BaseMemoryLib; all device accesses are naturally aligned 32-bit MMIO.

**Bluetooth** (UART-D 0x70006300, from the switch-linux DTS): `info`, `pwr` (PMIC 32 kHz out + pinmux), `bt [fw.hcd] [baud=N] [flow=1]`
(GPIO H4=BT_REG_ON, H3=DEV_WAKE, HCI Reset / Version / BD_ADDR / name, optional Broadcom patchram, optional baud switch),
`btscan [sec] [baud=N]` (classic Inquiry with RSSI/EIR names + LE active scan; put a Joy-Con/Pro Controller in SYNC mode; run after `bt`). No ACL/pairing and no EFI_BLUETOOTH protocol yet.

**Wi-Fi is PCIe, not SDIO** (switch-linux DTS: Tegra PCIe root port 2 / `pci@2,0`, x1 lane `pcie-0`; WIFI_EN=H0, WIFI_RST=H1, WIFI_WAKE=H2;
switch-linux builds brcmfmac with CONFIG_BRCMFMAC_PCIE only). The PCIe path is ported from Linux pci-tegra.c, xusb-tegra210.c (UPHY), clk-tegra210.c
(PLLE) and brcmfmac pcie.c/chip.c:
`pex [rst=0|1] [gen2=1]` (radio power, PMC partition, CAR clocks/resets, PLLE+UPHY PLL, AFI, root port, link training, BAR assignment),
`pchip [arm= wrap= d11= size= ram=]` (BAR0 backplane window, chip id, EROM dump, halt ARM CR4, TCM size),
`pfw fw.bin nvram.txt` (firmware + NVRAM into TCM, release CR4, wait for the shared-area pointer, dump it), `pcon` (firmware console log),
`pall fw nvram`, `poff`. `wifi`/`wfw` are the OLD SDIO experiment and are expected to fail on the Switch; kept only for reference.
Not implemented: msgbuf rings, scan/join, EFI_SIMPLE_NETWORK. Milestone for the first hardware test: `pfw` reports "FIRMWARE IS RUNNING" and `pcon` prints the firmware's own log.
ASSUMPTIONS (inferred, check in the log): WIFI_RST high = run (`rst=0` flips), RAM base 0x180000, ARM CR4 core 0x18002000 / wrapper 0x18102000,
d11 wrapper 0x18101000, PCIe window 0x13000000, CML0 gate = PLLE_AUX bit0. Firmware files are not included: take
`brcmfmac4356-pcie.bin/.txt` (and `BCM4356A2.hcd` for Bluetooth) from a Switch Linux install.

## License
GPL-2.0-only overall. See `LICENSE` and `NOTICE.winx.md` (components, hekate attribution, redistribution duties).

## Credits (Windows-for-Switch stack)
- [hayato41200](https://github.com/hayato41200) — Windows 10 on ARM for the Nintendo Switch: the XUSB/xHCI bring-up, USB-C PD and charging, CPU clock, fan, backlight and touch power code (`Drivers/UsbPowerAuditDxe`), the `SWOA*` ACPI devices, the XhciDxe write-combine patch, and the Windows kernel drivers and SwitchCenter app (`WindowsDrivers/`). Ported into winx as the `SWITCH_WOA` build.
  - [hayato41200/Windows-for-Switch](https://github.com/hayato41200/Windows-for-Switch) — UEFI / DSDT / bring-up notes (GPL-2.0).
  - [hayato41200/Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers) — Windows drivers and SwitchCenter (GPL-2.0).
