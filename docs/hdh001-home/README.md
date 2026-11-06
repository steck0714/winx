# HDH-001 Windows 11 Home

This branch integrates the tested Switch Lite SD path, an NTFS desktop boot path, and GPIO, I²C3 and Hoag FTM4 touch support. Select **Lite Windows 11 Home** in Hekate.

On 2026-11-05, one HDH-001 booted Simplified Chinese Windows 11 ARM64 Home 23H2 (22631.2861) from an 80 GiB NTFS WINARM volume. The owner could tap the Windows Setup error dialog. Setup phase 4 and windeploy subsequently completed successfully. The planned reboot before OOBE left the screen dark; manual startup subsequently reached the OOBE network-selection page. The BasicDisplay rollback later reached the desktop; the desktop three-circle touch check is still unconfirmed.

The latest offline SYSTEM hive records OOBEInProgress=0, SystemSetupInProgress=0, and SetupType=0. The media update preserves that completed state. The media update was deployed, then its display path was withdrawn after a black screen; see the display rollback below.

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

These last state changes are specific to this interrupted installation. Logs in `Windows/Panther` and `Windows/Panther/UnattendGC` determine whether a retry is appropriate. The next physical run completed phase 4 with return code 0, and windeploy also returned 0 after requesting a reboot before OOBE. SYSTEM then recorded GeneralizationState 7 and ChildCompletion/setup.exe 3. Preserve this successful state. The black screen was at the reboot boundary; see [the reset candidate](power-performance.md). Manual startup reached OOBE network selection, and the later saved hive cleared the setup flags as recorded above. The desktop touch check remains pending.

## Power-button development

Add `-D SWITCH_LITE_POWER=1` to the Lite touch build to expose I2C5 and the MAX77620 power-button device, ACPI SWOA0601. PmicDxe already enables and initializes I2C5. The existing tegra210i2c and switchpwrbtn drivers provide the bus and VHF System Power Down report. Hekate also reads the Hoag power button through MAX77620 ONOFFSTAT on I2C5. [Hekate button source](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/utils/btn.c)

The power-enabled firmware and ARM64/KMDF 1.15 power-button driver compile. Real button input and the Windows power action remain untested on this Lite. The Hekate entry continues to be **Lite Windows 11 Home**.

## Fan and clock configuration

The combined Lite build adds `-D SWITCH_LITE_POWER=1 -D SWITCH_LITE_FAN=1 -D SWITCH_FAN_DUTY=76 -D SWITCH_LITE_RESET=1 -D SWITCH_LITE_PERFORMANCE=1` to the base Home flags. Use [woa-lite-home-performance.ini](woa-lite-home-performance.ini) as `bootloader/ini/woa-lite-home.ini`; the entry name remains **Lite Windows 11 Home**.

`cpu_oc=1996800` requests 1996.8 MHz CPU. `ram_oc=2100000` prepares the 2100 MHz memory timing in Hekate and requests it through BPMP in firmware. Setting `cpu_oc=0` and `ram_oc=1600000` restores the normal boot profile on the next launch. Fan control is automatic; GPU overclocking is not implemented.

The firmware preserves the Hekate environment, identifies the MAX77812 variant, resolves a trained EMC rate through BPMP, checks the existing VDD_CORE supply against the Linux DVB voltage bands, and records the resulting clock in reserved memory. The shared `WindowsDrivers/drivers/SwitchFan` driver consumes this handoff. Its T214 CPU helper uses MAX77812 M4 at 1120 mV and PLLX for the requested 1996.8 MHz. It restores the boot frequency at 80 C or after repeated sensor failures and resumes below 70 C. A valid CPU overclock request keeps at least 51/236 fan duty even if clock preparation fails. CPU preparation is independent of the EMC result; voltage readback and temperature checks still apply.

Shared GPIO, I2C and fan sources remain in their upstream directories. The model-specific clock helper is `WindowsDrivers/drivers/SwitchFan/src/t214/`; firmware-specific handoff and reset code live under the corresponding ClockManagementDxe/PmicDxe `t214` directories. The existing Erista backlight behavior remains in the shared fan driver; the Lite resource maps only PWM1 and does not accept backlight control.

Build the status application with `Tools/LitePerformanceCheck/build.sh` and copy it to the public desktop as `LitePerformanceCheck.exe`. It reports runtime CPU/EMC frequency, temperatures, PWM duty and driver status. A green status requires fresh sensor updates and both requested frequencies; it is not a stability or fan-RPM test.

Validation: AARCH64 firmware and ARM64/KMDF builds, signed driver/catalog verification, the firmware fan-register comparison against Hekate, and host checks of the actual CPU helper's frequency calculation, voltage ordering, thermal hysteresis and failure handling. CPU 1996.8 MHz readback and PWM duty 51 were recorded on the next physical run. EMC 2100 MHz, sustained stability, physical fan response and restart remain unresolved. See [restart preparation](power-performance.md) for the Linux BootROM command table.

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

`LiteTouch/build-drivers.sh` now also builds the battery and RTC packages; the battery link requires the ARM64 WDK `battc.lib` in WDK_EXTRA_LIB. Keep the shared sources under `WindowsDrivers/drivers/SwitchBattery` and `SwitchRtc`. Place signed INF/SYS/catalog files in the matching subdirectories beside [Install-LiteSystem.cmd](Install-LiteSystem.cmd). Use `Install-LiteSystem.cmd offline W:\` from ARM64 maintenance WinPE, or `Install-LiteSystem.cmd online` from an elevated prompt in the existing Home installation. GPIO and I²C must already be installed. Replace W: with WINARM's actual drive letter. The current script stages fan, power-button, battery, RTC, and audio drivers without changing Windows setup state. It does not install a display miniport or landscape helper. Bluetooth is optional when its directory and firmware patch are supplied.

The Hekate entry remains **Lite Windows 11 Home**. Firmware and ARM64/KMDF builds pass. Signed SYS/catalog verification and PnPUtil staging of both packages passed in ARM64 WinPE 22621.1. Battery readings, RTC persistence, and physical volume-key input still await testing on this Lite.

Board references: [Switchroot Lite power tree](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-vali-power-tree.dtsi), [Lite button pinmux](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-vali-pinmux.dtsi), and [Hekate button inputs](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/soc/hw_init.c).

## Performance R1 hardware finding and correction

The first physical run recorded `CpuActualKhz=998400`, `HandoffStatus=3`, `RamActualKhz=0`, 292 temperature polls, 34 C SoC / 33 C board temperature, and fan duty zero. The temperature driver was running; the EMC handoff had failed, and the old `CpuConfigured` condition also disabled the requested fan floor. Zero RAM readback means unknown, not a measured zero clock.

The firmware now sends ATOMICS GET to trigger register 0x70016000 and reads result register 0x70016C00, following Switchroot `mail_t210.c`. It accepts and maps Hekate's CPU-accessible Mariko BPMP DRAM carveout at 0xFFE00000, in addition to the IRAM channel range. The previous code wrote to the result register and accepted only IRAM. `BpmpChannel` and an explicit initialization error now travel in the handoff. Shared SwitchFan 1.0.0.4 keeps the requested minimum fan duty and permits CPU setup independently of an EMC failure. The host CPU-helper regression covers that observed failure state.

The correction is built and deployed. The next physical trace reached CPU 1996.8 MHz and PWM duty 51. RAM 2100 MHz remains unresolved because the firmware found no matching EMC timing.

## Display rollback

The WinX/Microsoft KMDOD experiment supplies software framebuffer blits and rotation, not GPU rendering acceleration. Its ARM64 VM rotation check passed, but the first Lite media boot went black at display-device restart. The owner subsequently chose to drop the extra CPU rotation work and cancelled the proposed physical PE trial.

The active Home build omits `SWITCH_LITE_DISPLAY`. The targeted recovery removes `winxdisplay` and the `LiteLandscape` Run value/application from Windows. The normal installer no longer stages either component. The upstream display sources remain in their existing directory for reference; they are not part of the current Lite update. Home uses its previous native framebuffer orientation and Microsoft Basic Display. BCD, completed setup state, and the single Hekate entry were preserved and checked after the rollback; no PE trial is scheduled. The combined card update was safely ejected.

Saved records show display installation at the restart boundary with Kernel-PnP status 0xC00000E5. The audio endpoint was created. The fan trace recorded CPU 1996800 kHz, PWM duty 51, 34 C and no sensor failures. EMC status was `LitePerfNoTiming`, with 204000 kHz readback. These are hardware register/log observations, not proof of audible audio, fan RPM, sustained stability, or a working 2100 MHz memory target.

## Speaker and headphone audio

Add `-D SWITCH_LITE_AUDIO=1` with the Lite fan/I2C1 profile. `Drivers/UsbPowerAuditDxe/LiteAudio.inf` builds an audio-only entry from the existing upstream source. It prepares the APE power domain and clocks, initializes I2C1 explicitly, and calls the shared R403 codec/I2S/headphone setup. It does not invoke the Erista USB or CPU bring-up entry.

The Lite wiring matches Switchroot: RT5639 at I2C1 address 0x1C, codec LDO enable PZ4, I2S1/DAP1, and active-low headphone detection PV6. The Lite power tree supplies the speaker amplifier from the battery and codec rails from SD3; the port retains those board supplies. The shared Windows `switchaudio` 1.0.0.8 uses PortCls/WaveRT, ADMA channel 0, ADMAIF1, and 48 kHz / 16-bit stereo. Speakers and headphones share one Windows endpoint; the driver switches the codec output after jack changes. Microphone capture is not implemented by this upstream driver.

Build with `WindowsDrivers/drivers/SwitchAudio/build-arm64.sh`, using ARM64 WDK `portcls.lib` and `libcntpr.lib`. Sign the final package and inject `switchaudio.inf` alongside the fan and system-device packages. Firmware and driver builds pass; actual playback, jack switching, and audio quality remain unverified on HDH-001.

References: [Switchroot common audio wiring](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi), [Lite supplies](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-vali-power-tree.dtsi), [upstream R403 firmware](https://github.com/hayato41200/Windows-for-Switch/blob/main/Drivers/UsbPowerAuditDxe/UsbPowerAudit.c), and [Switchroot BPMP transport](https://github.com/CTCaer/switch-l4t-kernel-nvidia/blob/linux-5.1.2/drivers/firmware/tegra/mail_t210.c).

## Bluetooth on HDH-001

Add `-D SWITCH_LITE_BLUETOOTH=1` to the combined Home profile; it requires the Lite I2C5 power profile. `Drivers/UsbPowerAuditDxe/LiteBluetooth.inf` reuses the existing Bluetooth setup through a dedicated entry. It initializes I2C5, the PMIC GPIO4 32 kHz output, UART-D pins/clock, PH4 reset, and PH3 wake. It then checks HCI at 115200 before Windows takes ownership. The firmware exposes BTH0 (`ACPI\SWOA0B01`), UART-D at 0x70006300 and GSI 122.

The existing `WindowsDrivers/drivers/SwitchBluetooth` source is unchanged from upstream 1.0.0.13. The Lite driver build includes it. It loads the Broadcom patch, switches UART-D to 3 Mbaud, and attaches its radio child to Windows BthMini. The upstream delayed child creation and failed-boot recovery remain intact. Wait for the radio child to enumerate before checking Bluetooth Settings.

For installation, add the signed `switchbt` directory beside `Install-LiteSystem.cmd` and supply the Switch-specific patch as `Firmware/BCM4356A3.hcd`. The optional installer copies it to `Windows/System32/drivers/SwitchWOA` before staging the driver. The standard five-package system/audio kit works without the Bluetooth directory. The firmware blob is not committed to this repository. The local test kit uses Switchroot's `CYW4356A3_001.004.009.0092.0095.bin`, unchanged apart from the filename; it is 38,950 bytes and contains 192 HCD command records. This is a Bluetooth patch, not a Wi-Fi driver.

The combined firmware and ARM64 Bluetooth driver build. Signed package checks and native Windows staging pass. The BasicDisplay/audio/Bluetooth update is deployed, with the Bluetooth INF/CAT/SYS and patch read back and matched; radio discovery, keyboard/gamepad pairing, range, and restart behavior await physical testing. Upstream reports limited range without Wi-Fi-side antenna initialization, so a successful package install is not evidence of radio performance.

Switchroot's Lite device tree explicitly disables the host SPI4 controller because SIO owns it. The Erista `SwitchSensor` SPI4 profile is therefore left disabled; the Lite's built-in controls and motion data need the SIO transport. The native framebuffer orientation is retained.

References: [Lite SIO ownership](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/tegra210b01-vali.dts), [UART-D Bluetooth wiring](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi), [Switchroot patch provenance](https://gitlab.com/switchroot/android/proprietary_vendor_firmware/-/blob/main/README.md), and [upstream Bluetooth source and recovery tool](https://github.com/hayato41200/Windows-for-Switch-Drivers/tree/main/kits/Switch-Bluetooth).

## Ubuntu EMC comparison and RAM R2

The Lite Ubuntu path has three separate responsibilities. Hekate selects the MTC set by DRAM ID and prepares the requested `ram_oc` table through Arachne; BPMP owns the trained clock transitions. Linux calls CLK_ROUND_RATE before CLK_SET_RATE. The T210B01 DVFS path uses the DVB frequency/voltage bands, rather than requiring the requested number to occur as an exact entry in MRQ_EMC_DVFS_TABLE.

The earlier Home firmware required an exact 2100000 kHz row in that latter table and stopped at NoTiming, leaving a 204000 kHz readback. RAM R2 queries BPMP for a supported rate and checks it against the worst process-bin VDD_CORE requirement from Switchroot's normal/SLT tables. A 2100 MHz rate is covered by the 2133 MHz band (725 mV VDD_CORE). The existing rail is read, never raised; DRAM VDD2/VDDQ are not changed. A rounded rate above the user's request is rejected. A lower supported rate can be applied but remains reported as a target mismatch rather than a successful 2100 MHz overclock. The 48-byte Windows handoff ABI is unchanged.

The firmware build and focused policy regression pass. Physical 2100 MHz acceptance still depends on the next readback; this change does not invent or copy timing registers from another DRAM part.

References: [Hekate MTC/Arachne preparation](https://github.com/CTCaer/hekate/blob/master/bootloader/l4t/l4t.c), [Linux BPMP rounding](https://github.com/CTCaer/switch-l4t-kernel-nvidia/blob/linux-5.1.2/drivers/clk/tegra/clk-nv-bpmp.c), [T210B01 voltage bands](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/soc/tegra/tegra210b01-core-dvfs.c), and [DVB selection in Linux](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/soc/tegra/tegra210-dvfs.c).

## Speaker silence investigation and audio R2

The next physical Home run reached the desktop. Audio submitted a 12 KiB WaveRT buffer; ADMA completed 13 periods in the 100 ms snapshot and 177 periods before stop, with I2S consuming data. The owner reported silent internal speakers; headphones were not tested. This narrows investigation to the codec/output path but does not prove a cause.

The Lite firmware now uses Switchroot's NX codec private initialization: register 0x1D=0x0347, BIAS_CUR3=0x8AAA and class-D tuning 0x20=0x6110, alongside the existing common setup. The original Erista initialization remains selected outside LiteAudioInitialize. SwitchAudio 1.0.0.8 records codec power, mixers, clocks and private bias/tuning values after a jack-route change, without adding work to the render callback. The optional one-shot TestTone mechanism remains available for diagnosis, but it is disabled on the tested Home installation at the owner's request. The normal Windows startup sound is the first audible check.

This is a candidate correction based on the [Switchroot RT5640/RT5639 initialization](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/sound/soc/codecs/rt5640.c). Audible output remains pending.

On the existing offline Home installation, DISM Add-Driver staged 1.0.0.8 while the device still referenced 1.0.0.7. Native maintenance identified and removed the superseded non-boot-critical package, then retained the signed 1.0.0.8 package. The retained audio devnode is marked with CONFIGFLAG_REINSTALL (0x20) for PnP reinstallation at the next enumeration, using the [documented configuration flag](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/determining-whether-a-device-is-plugged-in). Windows PnP binding and audible playback must be checked at the next physical boot; staging alone is not reported as a running driver update.

## Pagefile setting for the tested Home installation

At the owner's request, the offline SYSTEM hive sets `ControlSet001\Control\Session Manager\Memory Management\PagingFiles` to the REG_MULTI_SZ value `C:\pagefile.sys 512 2048`: 512 MB initial, 2048 MB maximum. The prior setting was system-managed (`0 0`) with TempPageFile=1 and no persistent pagefile visible after shutdown. The new allocation is created by Windows at its next boot; the stale temporary-pagefile indicator is not forcibly cleared to hide a creation failure. Setup and BCD are preserved.

The native units and registry mapping are documented by [Win32_PageFileSetting](https://learn.microsoft.com/en-us/windows/win32/cimwin32prov/win32-pagefilesetting).

## R2 hardware follow-up: startup sound

The next physical boot bound audio 1.0.0.8, cleared the reinstall flag and read all recorded codec registers successfully. Jack=0 selected the speakers, SPK_VOL=0x1414 was unmuted, and ADMA completed 439 periods. These observations establish driver activation and data-path activity, not audible playback.

The owner clarified that only the Windows startup sound had been checked. The offline SOFTWARE hive had `Microsoft\Windows\CurrentVersion\Authentication\LogonUI\BootAnimation\DisableStartupSound=1`, with no overriding sound policy. The normal startup sound is enabled by setting this value to 0. The optional driver TestTone remains 0. No additional driver or firmware change is made for this configuration correction. Desktop playback and headphone output remain untested.

The same boot read CPU 1996.8 MHz and RAM 1866 MHz. BPMP rounded below the 2100 MHz request, so the firmware correctly reported a target mismatch rather than claiming 2100 MHz success.
