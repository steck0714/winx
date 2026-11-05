# HDH-001 Windows 11 Home

This branch integrates the tested Switch Lite SD path, an NTFS desktop boot path, and GPIO, I²C3 and Hoag FTM4 touch support. Select **Lite Windows 11 Home** in Hekate.

On 2026-11-05, one HDH-001 booted Simplified Chinese Windows 11 ARM64 Home 23H2 (22631.2861) from an 80 GiB NTFS WINARM volume. The owner could tap the Windows Setup error dialog. Setup phase 4 and windeploy subsequently completed successfully. The planned reboot before OOBE left the screen dark; manual startup subsequently reached the OOBE network-selection page. Desktop acceptance is pending.

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

These last state changes are specific to this interrupted installation. Logs in `Windows/Panther` and `Windows/Panther/UnattendGC` determine whether a retry is appropriate. The next physical run completed phase 4 with return code 0, and windeploy also returned 0 after requesting a reboot before OOBE. SYSTEM now records GeneralizationState 7 and ChildCompletion/setup.exe 3. Preserve this successful state. The remaining black screen is being investigated at the reboot boundary; see [the reset candidate](power-performance.md). Manual startup reached OOBE network selection. Offline continuation and the desktop touch check remain pending.

## Power-button development

Add `-D SWITCH_LITE_POWER=1` to the Lite touch build to expose I2C5 and the MAX77620 power-button device, ACPI SWOA0601. PmicDxe already enables and initializes I2C5. The existing tegra210i2c and switchpwrbtn drivers provide the bus and VHF System Power Down report. Hekate also reads the Hoag power button through MAX77620 ONOFFSTAT on I2C5. [Hekate button source](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/utils/btn.c)

The power-enabled firmware and ARM64/KMDF 1.15 power-button driver compile. Real button input and the Windows power action remain untested on this Lite. The Hekate entry continues to be **Lite Windows 11 Home**.

## Fan and clock configuration

The combined Lite build adds `-D SWITCH_LITE_POWER=1 -D SWITCH_LITE_FAN=1 -D SWITCH_FAN_DUTY=76 -D SWITCH_LITE_RESET=1 -D SWITCH_LITE_PERFORMANCE=1` to the base Home flags. Use [woa-lite-home-performance.ini](woa-lite-home-performance.ini) as `bootloader/ini/woa-lite-home.ini`; the entry name remains **Lite Windows 11 Home**.

`cpu_oc=1996800` requests 1996.8 MHz CPU. `ram_oc=2100000` prepares the 2100 MHz memory timing in Hekate and requests it through BPMP in firmware. Setting `cpu_oc=0` and `ram_oc=1600000` restores the normal boot profile on the next launch. Fan control is automatic; GPU overclocking is not implemented.

The firmware preserves the Hekate environment, identifies the MAX77812 variant, checks the trained EMC table and existing VDD_CORE supply, changes the EMC rate through BPMP and records the result in reserved memory. The shared `WindowsDrivers/drivers/SwitchFan` driver consumes this handoff. Its T214 CPU helper uses MAX77812 M4 at 1120 mV and PLLX for the requested 1996.8 MHz. It restores the boot frequency at 80 C or after repeated sensor failures, resumes below 70 C and keeps at least 51/236 fan duty while CPU overclocking is configured.

Shared GPIO, I2C and fan sources remain in their upstream directories. The model-specific clock helper is `WindowsDrivers/drivers/SwitchFan/src/t214/`; firmware-specific handoff and reset code live under the corresponding ClockManagementDxe/PmicDxe `t214` directories. The existing Erista backlight behavior remains in the shared fan driver; the Lite resource maps only PWM1 and does not accept backlight control.

Build the status application with `Tools/LitePerformanceCheck/build.sh` and copy it to the public desktop as `LitePerformanceCheck.exe`. It reports runtime CPU/EMC frequency, temperatures, PWM duty and driver status. A green status requires fresh sensor updates and both requested frequencies; it is not a stability or fan-RPM test.

Validation: AARCH64 firmware and ARM64/KMDF builds, signed driver/catalog verification, the firmware fan-register comparison against Hekate, and host checks of the actual CPU helper's frequency calculation, voltage ordering, thermal hysteresis and failure handling. Physical frequency, fan response and restart validation are still pending. See [restart preparation](power-performance.md) for the Linux BootROM command table.

## Battery, RTC, and volume buttons

Add `-D SWITCH_LITE_SYSTEM=1` to the combined fan/power build above. This exposes the existing upstream device definitions on HDH-001, with dependencies on the GPIO and I²C controllers:

| Device | Resources | Windows driver |
|---|---|---|
| BAT0, SWOA0301 | I2C1: MAX17050 at 0x36 and BQ24193 at 0x6B | Shared `switchbattery` 1.0.0.6 |
| RTC0, SWOA0801 | I2C5: MAX77620 RTC at 0x68 | Shared `switchrtc` 1.0.0.2 |
| BTNS, ACPI0011 | PX6 / GPIO 190 and PX7 / GPIO 191, both-edge interrupts | Windows inbox `hidinterrupt.inf` through `tegra210gpio` |

BAT0 supplies the upstream driver's two-connection path: battery percentage, voltage, temperature, and charger status. It omits the BM92T connection, so this profile does not start the driver's USB-C role-switching timer or change charger settings. This is battery telemetry; USB-C host/charging integration is a separate porting step.

RTC uses the upstream Windows-only offset. After a manual time change or a successful time synchronization, it records `OffsetSeconds` and `OffsetFormat=2` under `Services\switchrtc\Parameters`. Subsequent starts restore RTC time plus that offset. It latches the RTC read registers but does not rewrite the calendar shared with Horizon. With no learned offset, it leaves Windows time unchanged.

The volume keys reuse the upstream ACPI button mapping. Hekate configures PX6/PX7 as inputs; the Lite GPIO driver already exposes the corresponding bank interrupt. No separate volume-button binary is needed. Working volume-button input does not imply audio playback support.

`LiteTouch/build-drivers.sh` now also builds the battery and RTC packages; the battery link requires the ARM64 WDK `battc.lib` in WDK_EXTRA_LIB. Keep the shared sources under `WindowsDrivers/drivers/SwitchBattery` and `SwitchRtc`. Place signed INF/SYS/catalog files in the matching subdirectories beside [Install-LiteSystem.cmd](Install-LiteSystem.cmd). Use `Install-LiteSystem.cmd offline W:\` from ARM64 maintenance WinPE, or `Install-LiteSystem.cmd online` from an elevated prompt in the existing Home installation. GPIO and I²C must already be installed. Replace W: with WINARM's actual drive letter. The script stages fan, power-button, battery, and RTC drivers without changing Windows setup state.

The Hekate entry remains **Lite Windows 11 Home**. Firmware and ARM64/KMDF builds pass. Signed SYS/catalog verification and PnPUtil staging of both packages passed in ARM64 WinPE 22621.1. Battery readings, RTC persistence, and physical volume-key input still await testing on this Lite.

Board references: [Switchroot Lite power tree](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-vali-power-tree.dtsi), [Lite button pinmux](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-vali-pinmux.dtsi), and [Hekate button inputs](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/soc/hw_init.c).
