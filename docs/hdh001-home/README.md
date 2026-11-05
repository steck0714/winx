# HDH-001 Windows 11 Home

This branch integrates the tested Switch Lite SD path, an NTFS desktop boot path, and GPIO, I²C3 and Hoag FTM4 touch support. Select **Lite Windows 11 Home** in Hekate.

On 2026-11-05, one HDH-001 booted Simplified Chinese Windows 11 ARM64 Home 23H2 (22631.2861) from an 80 GiB NTFS WINARM volume. The owner could tap the Windows Setup error dialog. First-run configuration is still being repaired. Desktop use and the three-point TOUCH PASS test remain pending.

## Firmware build

Use the EDK2 and devkitA64 build environment from the [WinPE source recipe](../hdh001-lab/README.md). This branch builds a desktop firmware using the same per-card identity header as the Windows SD miniport.

1. Generate the local header from a saved whole-card image with the existing target generator. Its supported layout is described in the [R3 notes](../hdh001-winarm-r3/README.md).

   ```sh
   target=docs/hdh001-home/generated
   python3 WindowsDrivers/drivers/Tegra210Sdhc/winarm/configure-target.py /path/to/saved-card.img "$target"
   cp "$target/winarm-target.h" Include/
   cp "$target/winarm-target.h" WindowsDrivers/drivers/Tegra210Sdhc/src/
   ```

2. Download the read-only ARM64 NTFS-3G UEFI 1.9 driver to `LiteSdTest/ntfs_aa64_ro.efi`. Its SHA-256 is `c04d008d79b4add54a2a770fb904b0df89ae7b1ea63d2d7e5d273145f4106a45`. [Driver release](https://github.com/pbatard/ntfs-3g/releases/tag/1.9)

3. Build RELEASE AARCH64 with `-D SWITCH_HEKATE=1 -D SWITCH_FB_ROTATE=2 -D SWITCH_EXCFB=1 -D SWITCH_ACPI_SD_NVTSDHC=1 -D SWITCH_LITE_TOUCH=1`. Pack BL33 using `Tools/make-bl33.py` and the corresponding FV map. Put BL31 and BL33 in `switchroot/woa-lite-home/`, then copy [woa-lite-home.ini](woa-lite-home.ini) to `bootloader/ini/`.

The SD firmware verifies the physical MBR, GPT and NTFS identities, then exposes an in-memory MBR view containing the configured WINARM partition. The firmware and nvtsdhc 0.3.2.5 both mark this presented partition active. The stored partition tables are used to identify the card.

## Windows drivers

Build the SD miniport with [build-miniport.sh](../hdh001-lab/build-miniport.sh). Build the GPIO, I²C and touch packages with [LiteTouch/build-drivers.sh](../../LiteTouch/build-drivers.sh). Sign the final SYS files, generate catalogs from the final INF/SYS pairs with MakeCat, and sign the catalogs with your development certificate. The installed image needs that certificate in Root and TrustedPublisher and a BCD loader entry with test signing enabled.

On an offline ARM64 Windows image, install all four INF packages through DISM. Keep nvtsdhc, sdbus and sdstor as boot-start services. The SD INF selects ACPI NVDA0101, and the touch profile exposes SWOA0101, SWOA0201 and SWOA0502.

Build [touch-check.c](../../LiteTouch/touch-check.c) with [build-check.sh](../../LiteTouch/build-check.sh). Copy its output to `Users/Public/Desktop/LiteTouchCheck.exe` in the offline image. After Windows first-run configuration finishes, tap and release the three circles. TOUCH PASS means the application observed the expected touch pointer events.

## First-run findings

- The initial EfiFs driver opened `bootmgfw.efi` but returned Volume Corrupt on the first read. On the same card exposed read-only to ARM64 UEFI, NTFS-3G read the loader, BCD and winload.efi successfully and loaded the complete boot manager.
- Setup initially reached specialize but failed in `Sysprep_Online_Specialize_Bcd` with `0xC0000452`. The inactive MBR partition was rejected by the system-partition validation routine. The presented active bit is the corresponding firmware/SD change.
- The failed run left `GeneralizationState=6`, which caused windeploy to stop with `0x8030000C`.
- Restoring GeneralizationState to 4 exposed the next stale setup state. `ChildCompletion/setup.exe=1` caused an unexpected-restart dialog and exit code `0x1f`. The original Home image has no such value and uses SetupType 1. Restore the original first-run state when retrying this interrupted installation, after preserving the current SYSTEM hive.

A subsequent log showed that Setup tried to resume an installation whose saved MainQueueOnline was absent. The recovery now archives the Panther setup record and clears Setup/WorkingDirectory so the next boot can build a new first-boot queue.

These last state changes are specific to this interrupted installation. Logs in `Windows/Panther` and `Windows/Panther/UnattendGC` determine whether a retry is appropriate. Completion of specialize and OOBE still needs the next hardware result.

## Power-button development

Add `-D SWITCH_LITE_POWER=1` to the Lite touch build to expose I2C5 and the MAX77620 power-button device, ACPI SWOA0601. PmicDxe already enables and initializes I2C5. The existing tegra210i2c and switchpwrbtn drivers provide the bus and VHF System Power Down report. Hekate also reads the Hoag power button through MAX77620 ONOFFSTAT on I2C5. [Hekate button source](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/utils/btn.c)

The power-enabled firmware and ARM64/KMDF 1.15 power-button driver compile. Real button input and the Windows power action remain untested on this Lite. The Hekate entry continues to be **Lite Windows 11 Home**.

## Fan and clock configuration

The requested clock profile is approximately 2 GHz CPU and 2100 MHz memory, enabled through a configuration file. Deploy it together with the Lite fan-control implementation. GPU overclocking is deferred until the graphics driver is ready. This power-button change does not apply that profile: the Home INI retains `ram_oc=1600000` and no CPU voltage or clock writes are added.

Mariko uses MAX77812 M4 for the CPU rail; the existing Erista SwitchCpuClock/MAX77621 profile must not be enabled on this Lite. Hekate's `ram_oc` table preparation also needs to be paired with a verified runtime memory-clock transition before reporting the requested memory frequency as active.
