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

## Known caveats
* Windows on a Cortex-A57 (ARMv8.0): recent Windows 11 builds need ARMv8.1 atomics, so use Windows 10 ARM64
  or an older Windows 11 build (about 23H2 and earlier).
* The ACPI tables only describe CPU/GIC/timer/UART at the moment; Windows has no SD, USB, GPU, input drivers yet.
* After `ExitBootServices` the firmware USB/PD handling stops.

## License
GPL-2.0-only overall. See `LICENSE` and `NOTICE.winx.md` (components, hekate attribution, redistribution duties).
