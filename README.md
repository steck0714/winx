
# WinX

<img width="1536" height="1024" alt="winx" src="https://github.com/user-attachments/assets/6c031e07-31b2-4030-8395-54d8e745d450" />

UEFI firmware for NVIDIA Tegra210 (T210) platforms.

WinX is derived from [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg) and uses the 202402-EDK2 build.

## Status

Capable of booting from an SD card through Hekate using a BL31/BL33 firmware setup.

A Linux kernel with EFI stub support should boot when supplied with a compatible device tree for a T210-based device.

ACPI-based booting also supports Windows and Linux. Available device support depends on the firmware configuration and the drivers provided.

## Working Features

The following features are reported as working in the supplied project notes:

- USB keyboard input.
- SD card access.
- USB flash drive access.
- Display output.
- UEFI Shell memory access through `mm`.
- UEFI Shell device listing through `devices`.
- Filesystem mapping through `map`.
- Filesystem access through mappings such as `fs0:`.
- FAT32 filesystem access.
- Windows Boot Manager (`bootmgfw.efi`) starts and loads a 412 MiB `boot.wim` from the SD card (see Windows Boot Status).
- Windows 11 ARM64 boots from an NTFS partition on the SD card (FAT32 holds `bl33.bin` and the boot files; see Windows Boot Status, 2026-10-28).
- Read-only NTFS / exFAT / ext2-4 / XFS / Btrfs / F2FS / EROFS access in UEFI via EfiFs-based drivers (`README.fsdrv`); NTFS confirmed on hardware.

## Windows Boot Status (2026-10-28, one Erista console)

Reported by the author from real-hardware tests. Items not listed here are untested.

| Stage | Result |
|---|---|
| hekate -> BL31 -> bl33 -> UEFI Shell, SD (FAT32) read | works |
| NTFS partition on the SD card readable from UEFI (EfiFs NTFS driver) | works |
| Windows 11 ARM64 started from the NTFS partition (FAT32 partition: `bl33.bin` etc.) | **boots** |
| Booting while the console is charging | **BSOD** (unresolved) |
| xHCI (XUSB) reaches Windows | partly: Windows sees it, then **BSOD** |
| Custom EHCI / xHCI path | works in UEFI only; no working USB in Windows yet |
| Touch panel (`TCH0` / `touch-fts4`) in Windows | **no response** (no BSOD either) |
| Windows keyboard / Wi-Fi / Bluetooth / sound / GPU acceleration | not working yet |

Next steps: get the BSOD bugcheck code (kernel debugger below, or the memory dump), compare the `XHC0` / `EHC0` / `TCH0` / `I2C*`
entries in `AcpiTables/Dsdt/Dsdt.asl` with what the Windows drivers expect, then rerun the A/B EHCI test in `TEST_PLAN.md`.
Note: recent Windows 11 builds need ARMv8.1 atomics (Cortex-A57 is ARMv8.0); the author's working image is not a 24H2-era one.

## Windows Boot Status - older notes (2026-10-19, WinPE from a RAM disk; kept for history)

| Stage | Result |
|---|---|
| hekate -> BL31 -> bl33 -> UEFI Shell, SD (FAT32) read | works |
| `bootmgfw.efi` (ARM64 Windows Boot Manager) started from the Shell or `\EFI\BOOT\BOOTAA64.EFI` | works (`bootmgr.efi` is not accepted; use the `bootmgfw.efi` name) |
| Loading `\sources\boot.wim` (about 412 MiB) into a RAM disk | works with the SD fix in this tree (no more `0xc0000185`) |
| Windows logo | shown, then the screen **stayed there (hang)** with the 24H2-era WinPE (later: Windows 11 itself boots, see above) |
| WinPE desktop / command prompt | not reached at that time |

What was found on the way (details for anyone retracing this):

* **WinPE files must be the unpacked ISO contents at the root of the FAT32 volume** (`\EFI\Microsoft\Boot\BCD`,
  `\EFI\Microsoft\Boot\bootmgfw.efi`, `\sources\boot.wim`, `\boot\boot.sdi`).
  The Windows key in the boot manager asks for a firmware reboot (back to hekate); the keyboard may not respond there.
* **`0xc0000185` ("a required device isn't connected or can't be accessed") came from the SD driver.** A single
  transient SDMMC transfer error left the host controller wedged, so every later read returned `EFI_DEVICE_ERROR`;
  the boot manager hits that while reading `boot.wim`. Reproduced with a read test: the first 4 KiB-chunk pass failed
  at about 37 MiB and all later passes failed from offset 0. The driver now resets the CMD/DAT lines after an error,
  stops the card, waits for it to become ready and retries (up to 5 times; the clock is lowered to 25 MHz after the second
  failure) and uses a persistent 512 KiB DMA bounce buffer allocated at start-up instead of allocating pages per request.
  With this, the whole 432 MB `boot.wim` reads back with identical hashes for 4 KiB / 64 KiB / 1 MiB / 8 MiB chunks.
* **SD log:** the driver keeps a log in the volatile variable `SdDiag`
  (GUID `5D69E400-E850-4A90-8B9B-E97A8C22237D`; Shell: `dmpstore SdDiag -guid 5D69E400-E850-4A90-8B9B-E97A8C22237D`
  or `dmpstore -all`; a plain `dmpstore SdDiag` only searches the global GUID and finds nothing). It lists the bring-up steps and every transfer error with the SDHCI
  status (`sts=`, error bits 31:16).
* The microSD is **read-only** from UEFI (writes are not implemented), so a script cannot write logs to the card.
* A read pass over a 432 MB file takes about 67 s (about 6.4 MB/s) in the safe build; the boot manager needs about the
  same time to load `boot.wim`, so a black-looking screen for 1-2 minutes is normal.

### SD builds

* **Safe** (default, `-D SWITCH_SD_FAST` not set): error recovery and retry, persistent bounce buffer, no per-command
  delay. `release/2/bl33.bin` is a safe build supplied by the author and **tested on hardware** (it is not a
  rebuild of this exact tree; builds are not bit-reproducible).
* **Fast** (`-D SWITCH_SD_FAST=1`): additionally 128 KiB requests, Auto CMD12 and the SDHCI High Speed bit.
  **Untested.** If you try it, run a whole-file read test (hash compare) before booting Windows.
* `release/1/bl33.bin` is the older basic release (no SD fix); `release/2/bl33.bin` is the newer one. Same file names, separate folders (`patches/1`, `patches/2` likewise).

### Why the old WinPE test stopped at the logo (not verified; historical)

* The WinPE used for the tests (TPE-ARM-64) is a Windows 11 24H2 era image (boot manager 10.0.26089, WIM 10.0.26100).
  The Switch's Cortex-A57 is ARMv8.0 and has no LSE atomics, which recent Windows 11 kernels need. A hang or crash right
  after the logo is what that would look like. Try Windows 10 ARM64 or a 23H2-or-older Windows 11 WinPE.
* The ACPI tables describe only CPU, GIC, timer, UART and the SD controllers. Windows has no storage, USB, GPU or
  input driver for this hardware yet. A WinPE booted from a RAM disk does not need the SD card after loading, but it
  cannot be used without input devices.
* Next steps: enable the kernel debugger (see below) and watch the serial port, then test an older WinPE.

## Device Support

- **CPU services:** GIC and ARM Generic Timer.
- **Clocks:** Clock, reset, PLL, and related services.
- **Power management:** PMC, PMIC, regulators, and related components.
- **GPIO and pin multiplexing.**
- **MicroSD:** SDSC and SDHC support. SDXC has been probed and its partition table detected (a 125 GB card is used for the Windows tests). Read-only from UEFI unless the `sd-write` change is built in (untested). Transfer errors are recovered and retried (see Windows Boot Status).
- **eMMC:** I don't have any plans to provide support at the moment.
- **Display and framebuffer:** Display output is working in the documented setup. The framebuffer path requires a compatible Coreboot implementation.
- **Sideband buttons:** Not yet registered as UEFI input devices.
- **UART:** 115200 baud, 8N1, through the right-side rail connector.
- **ACPI:** Platform device descriptions, including SD/MMC controllers.
- **RTC and time:** Real RTC via the MAX77620 with `-D SWITCH_RTC=1` (see `README.winx.md`); without it `date` / `time` use the build time and are lost after power-off.
- **SMBIOS:** Platform information and clock-measurement support.
- **Fan control:** Fan-control driver integration.
- **USB 2.0:** Available, but may stop working after a voltage or connection-state change, such as disconnecting a keyboard while charging.

## UEFI Shell Limitations

- **FAT32:** Supported. exFAT has not been tested.
- **RAM disk:** Not usable because the required driver is absent.
- **PCI:** Not usable because the PCI bus driver is absent.
- **Networking:** Not usable because the underlying networking drivers are absent.
- **USB network adapters:** No adapter driver is included.
- **ACPI View:** The `acpiview` command requires `UefiShellAcpiViewCommandLib` to be included in the shell build.

To enable `acpiview`, add the following entry to the shell configuration:

```text
NULL|ShellPkg/Library/UefiShellAcpiViewCommandLib/UefiShellAcpiViewCommandLib.inf
```

## Hekate Configuration

Add the following entry to `bootloader/hekate_ipl.ini` on the SD card:

```ini
[Windows ARM64]
l4t=1
boot_prefixes=switchroot/winx/
id=WINXHKT
```

Place the firmware files in the following directory:

```text
switchroot/
└── winx/
    ├── bl31.bin
    └── bl33.bin
```

The supplied project notes report using the `bl31.bin` image from LineageOS 22.2, specifically the `nx_tab` build.

Other LineageOS builds and Ubuntu images were mentioned as possible alternatives, but were not confirmed in the supplied notes.

## Windows Kernel Debugger Enablement

```powershell
bcdedit /store E:\EFI\Microsoft\Boot\BCD /set {default} debug on
bcdedit /store E:\EFI\Microsoft\Boot\BCD /dbgsettings serial debugport:1 baudrate:115200
```

Replace the BCD store path and serial port settings as necessary.

Connect the serial interface through the right-side rail connector to a compatible host and configure WinDbg for serial kernel debugging at 115200 baud.

## Planned / In-Progress

- Fix the BSOD when booting while charging, and the xHCI BSOD in Windows.
- Make touch work in Windows (currently no response).
- Windows keyboard (USB / EHCI via ACPI, see `TEST_PLAN.md`), Wi-Fi, Bluetooth, sound.
- Test the fast SD build (`SWITCH_SD_FAST=1`).
- EHCI / USB 3.0 host support.
- eMMC support?
- Sideband buttons as UEFI input devices.
- Mouse support.
- Network stack and USB network adapter drivers.
- Additional UEFI Shell commands and utilities, including `acpiview`, `dp`, `tftp`, `ifconfig`, and `ping`.

## Building

See [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg). The build environment and workflow are almost identical!

WinX is based on [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg), with additional T210-specific firmware integration and modifications.

Refer to Lumia950XLPkg for the general build workflow, and consult the WinX and NintendoSwitchPkg source trees for platform-specific configuration, dependencies, and build targets.

## Credits

- [hayato41200](https://github.com/hayato41200) — Windows 10 on ARM for the Nintendo Switch: the XUSB/xHCI bring-up, USB-C PD and charging, CPU clock, fan, backlight and touch power code (`Drivers/UsbPowerAuditDxe`), the `SWOA*` ACPI devices, the XhciDxe write-combine patch, and the Windows kernel drivers and SwitchCenter app (`WindowsDrivers/`). Ported into winx as the `SWITCH_WOA` build.
  - [hayato41200/Windows-for-Switch](https://github.com/hayato41200/Windows-for-Switch) — UEFI / DSDT / bring-up notes (GPL-2.0).
  - [hayato41200/Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers) — Windows drivers and SwitchCenter (GPL-2.0).
- [NintendoSwitchPkg](https://github.com/steck0714/NintendoSwitchPkg) — Base firmware package.
- [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg) — Build environment reference.
- [EDK II](https://github.com/tianocore/edk2) — UEFI development framework.
- [Hekate](https://github.com/CTCaer/hekate) — Bootloader and configuration integration.
- [Switchroot](https://switchroot.org/) — Source of compatible BL31 firmware through Switchroot/LineageOS packages. BL31 is supplied separately and is not included in this repository.

## Windows-for-Switch stack (`SWITCH_WOA`)

`Tools/build-woa.sh` builds winx with the hardware bring-up from
[hayato41200/Windows-for-Switch](https://github.com/hayato41200/Windows-for-Switch) instead of the EHCI stack:
`UsbPowerAuditDxe` (XUSB firmware load, xHCI, USB-C PD / charging, CPU clock, fan, backlight, touch power),
the stock `XhciDxe` (with `patches/edk2/0001-*.patch`), and the `SWOA0101..0801` ACPI devices. The matching Windows
drivers and the SwitchCenter app from
[hayato41200/Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers) are in `WindowsDrivers/`.
Upstream READMEs and bring-up notes are kept in `docs/hayato41200/`. See `docs/SWOA-PORT-ja.md` for what was changed and what is untested.
You must supply `Firmware/tegra21x_xusb_firmware` (see `Firmware/README.md`). Experimental; not verified on hardware in the hekate boot chain.

## Disclaimer

WinX is experimental firmware. The working features listed above reflect the supplied project notes. Other functionality may be incomplete, limited, or untested. Operating-system compatibility and hardware behavior may vary by configuration.
