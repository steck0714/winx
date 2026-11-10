# Original WinX notes for Nintendo Switch T210 (Erista)

These notes describe the original Hekate and EDK II port on an Erista console. The author reported booting Windows 11 ARM64 from an NTFS partition on 2026-10-28. The hardware results below apply to that console and firmware revision.

Evidence labels and the distinction between physical reports, build results and source-derived theoretical values are defined in [verification status](docs/verification-status.md). These historical Erista reports have not been reproduced on the current source build.

The patch set was based on NintendoSwitchPkg commit `09c3ade` and `edk2-stable202402`. [Original WinX project](https://github.com/steck0714/winx) · [Base firmware](https://github.com/imbushuo/NintendoSwitchPkg) · [Switch Lite hardware status](README.md#hardware-status)

## Status (reported from a real Erista console)
| | |
|---|---|
| hekate -> BL31 -> bl33 -> UEFI Shell | Physical report: reached the shell on the author's Erista |
| Shell orientation | Physical report on that unit with `SWITCH_FB_ROTATE=2`; not a four-model result |
| USB host + **USB keyboard** in the Shell | Physical report: USB 2.0 keyboard worked; one Dell keyboard via Type-C OTG was tested. No broad keyboard-compatibility claim |
| microSD visible (`map`) after hekate | Physical report: SD was accessible in UEFI with the SDMMC power-up patch |
| `startup.nsh` auto-run | Unverified |
| Windows 11 ARM64 from NTFS on the SD card | Owner-reported boot on 2026-10-28; charging/xHCI failures and no touch response were also reported. This is not a stability or all-SKU result. |
| NTFS read-only in UEFI (`SWITCH_FSDRV`) | Physical report for NTFS only. Other EfiFs formats are not implied to work. |
| Windows PE/installer as a separate test; USB mass storage; PD/charging side effects | Unverified or not separately tested |

Not verified: Mariko / Lite / OLED consoles. Only the author's console was used.

## What you need to supply
* hekate (GPL-2.0), `bl31.bin` (Arm Trusted Firmware for Tegra210, e.g. from a switchroot/LineageOS package).
* The historical image `release/archive/edk2-stable202402-fsdrv/bl33.bin`, or build the current source yourself.

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
`Tools/build-full.sh` builds the integrated source configuration (including `SWITCH_XUSB` and `SWITCH_WIFIBT`); a successful build proves compilation and image generation only. Hardware test scripts for the SD card are in `tests/` (`winx_test.nsh`, `xusb_test.nsh`, `wifi_test.nsh`);
each experiment writes a log file (`xusb.log`, `wifibt.log`) whose LAST line names the failing step. Everything marked DRAFT is unverified on hardware.

## Known caveats
* **Windows image compatibility is version-specific.** Microsoft's minimum hardware requirements list ARMv8.1 atomic instructions for ARM64, while Arm's Cortex-A57 manual describes the core as Armv8-A. This is a compatibility constraint, not proof that every Windows 11 image fails to boot. The Lite report records Windows 11 build 22631.2861 (23H2); other image versions need their own test. [Microsoft requirements](https://download.microsoft.com/download/7/8/8/788bf5ab-0751-4928-a22c-dffdc23c27f2/Minimum%20Hardware%20Requirements%20for%20Windows%2011.pdf) · [Arm Cortex-A57 manual](https://documentation-service.arm.com/static/5e906b9fc8052b1608760c7b)
* The ACPI tables now also describe SD, XHC0/EHC0, I2C, touch, battery, fan, RTC and the `SWOA*` devices (see `AcpiTables/Dsdt/Dsdt.asl`); the matching Windows drivers are in `WindowsDrivers/` and mostly unverified.
* After `ExitBootServices` the firmware USB/PD handling stops.

## Optional feature switches (build options; runtime status must be checked separately)
Pass as extra `-D NAME=1` to `build` (together with `SWITCH_HEKATE=1 SWITCH_USB=1 SWITCH_FB_ROTATE=2`). The FD stays at
0xE0000 (bl33.bin = 921600 bytes) for every combination below; `SWITCH_BIGFD=1` grows it to 0xFF000 if it ever stops fitting.

Hardware register/clock values below are source-derived or theoretical unless this file explicitly records a physical readback.

| Switch | Adds | Notes |
|---|---|---|
| (always) | `acpiview` shell command | `UefiShellAcpiViewCommandLib` linked into the Shell |
| `SWITCH_EXT` | USB mouse (`UsbMouseDxe`) + `MouseCursorDxe` (software arrow cursor on the GOP with acceleration, red/blue while left/right button is held, hidden until the first mouse event; the SimplePointer events are shared with the Shell editors/HII, not stolen), `RamDiskDxe` | RAM disks are created from the HII form (UiApp > Device Manager > RAM Disk Configuration); EDK2's shell has no `ramdisk` command |
| `SWITCH_RTC` | RTC-backed `date` / `time` code path | MAX77620/I2C address and behavior are source-derived; runtime read/write on Switch is unverified. |
| `SWITCH_NET` | UEFI IPv4 stack (Mnp/Arp/Ip4/Udp4/Dhcp4/Mtftp4), USB NIC drivers CDC-ECM, CDC-NCM, RNDIS, `tftp` | `ifconfig` / `ping` already live in the Shell (Network1 lib); they need a NIC to be useful |
| `SWITCH_XUSB` | `xusbinit` | see below |
| `SWITCH_WIFIBT` | `wifibt` | see below |
| `SWITCH_EXCFB` | exception dump on the panel | The handler is included in the RELEASE build and its renderer has a host test. No physical exception/display test is recorded. |
| `SWITCH_ACPI_XHCI` | XUSB xHCI resource description in DSDT | Adds `XHC0`; address/size/INTID are source-derived theoretical resources. AML/build inspection does not prove XUSB responds. Requires firmware and mailbox service; off by default. |
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
THEORETICAL SOURCE-DERIVED ASSUMPTIONS (not physical readbacks; check any future log): WIFI_RST high = run (`rst=0` flips), RAM base 0x180000, ARM CR4 core 0x18002000 / wrapper 0x18102000,
d11 wrapper 0x18101000, PCIe window 0x13000000, CML0 gate = PLLE_AUX bit0. Firmware files are not included: take
`brcmfmac4356-pcie.bin/.txt` (and `BCM4356A2.hcd` for Bluetooth) from a Switch Linux install.

## License
GPL-2.0-only overall. See `LICENSE` and `NOTICE.winx.md` (components, hekate attribution, redistribution duties).

## Credits (Windows-for-Switch stack)
- [hayato41200](https://github.com/hayato41200) — Windows 10 on ARM for the Nintendo Switch: the XUSB/xHCI bring-up, USB-C PD and charging, CPU clock, fan, backlight and touch power code (`Drivers/UsbPowerAuditDxe`), the `SWOA*` ACPI devices, the XhciDxe write-combine patch, and the Windows kernel drivers and SwitchCenter app (`WindowsDrivers/`). Ported into winx as the `SWITCH_WOA` build.
  - [hayato41200/Windows-for-Switch](https://github.com/hayato41200/Windows-for-Switch) — UEFI / DSDT / bring-up notes (GPL-2.0).
  - [hayato41200/Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers) — Windows drivers and SwitchCenter (GPL-2.0).

## R392 sync with hayato41200/Windows-for-Switch
`Drivers/UsbPowerAuditDxe` is now hayato41200's R392 (was R379), and the DSDT carries his R389/R390 changes: `BTH0` (`SWOA0B01`, for `switchbt.sys`),
`XSTA` (XHC0 stays hidden until XUSB is really running, patched by UsbPowerAuditDxe), and 20 ms volume-button debounce. This matches the drivers in
Windows-for-Switch-Drivers (SwitchDrivers 2026-10-31). The XUSB firmware is no longer embedded by default (NVIDIA licence): put `tegra21x_xusb_firmware`
(124,416 B L4T build, tested) in the root of the SD card's ESP, or build with `-D EMBED_XUSB_FIRMWARE=TRUE` after placing it in `Firmware/`.
Build: `Tools/setup_efifs.sh <edk2>` then `Tools/build-woa.sh -D SWITCH_FSDRV=1` (add `-D SWITCH_ACPI_SD_NVTSDHC=1` for the nvtsdhc SD driver, Windows 11 22621+).
Prebuilt: `release/R392/bl33.bin` (inbox sdbus, Windows 10) and `release/R392/bl33_nvtsdhc.bin`.

## R402 sync (audio bring-up)
`Drivers/UsbPowerAuditDxe` is hayato41200's R402 and the DSDT has `AUD0` (`SWOA0C01`, for switchaudio.sys). UEFI powers the codec (RT5639 on I2C1 0x1C),
the APE clocks and I2S1 and leaves the audio path running; no tone is played at boot (`SWITCH_AUDIO_TEST_TONE 0`).
Build variants (all with `-D SWITCH_HEKATE=1 -D SWITCH_BIGFD=1 -D SWITCH_FB_ROTATE=2 -D SWITCH_FSDRV=1 -D SWITCH_ACPI_SD_NVTSDHC=1`):
- `bl33_R402_woa_nvt_efifs.bin`: `-D SWITCH_WOA=1` (R402, audio, Bluetooth, touch power, XUSB from the ESP)
- `bl33_R392_woa_nvt_efifs.bin`: same with R392 (no audio)
- `bl33_R392_base_nvt_efifs.bin`: no `SWITCH_WOA` (no UsbPowerAuditDxe); `-D SWITCH_USB=1`. The SWOA DSDT devices exist but nothing powers touch, fan, etc.
Hekate chain: Windows may hang at the logo with no blue screen; see the R386/R388/R389 notes in hayato41200/Windows-for-Switch.

### One ROM for Windows 10 and Windows 11 (`-D SWITCH_ACPI_SD_AUTO=1`)
`Drivers/SdAutoDxe` runs just before the boot option starts, connects the block devices and looks for `\Windows\System32\drivers\nvtsdhc.sys`
(NTFS needs `-D SWITCH_FSDRV=1`). It patches `SDMD` in the DSDT (`Name (SDMD, 0x10)`): `0x02` nvtsdhc found (SDM1 = NVDA0101 / NVD0D40),
`0x01` a Windows without it (NVDA0212 / PNP0D40, inbox sdbus), unchanged `0x10` when no Windows volume was read: the DSDT then uses
`_OSI ("Windows 2021")` (Windows 11 yes, Windows 10 no). The screen shows one `SdAuto: ...` line. `_CCA` is 0 with nvtsdhc and 1 with sdbus (UNTESTED).
So: install nvtsdhc (and its certificate) into a Windows 11 image and leave it out of a Windows 10 image; the ROM is the same.
