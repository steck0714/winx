# HDH-001 shutdown and restart

## Current result

On 2026-11-06, the owner confirmed that **normal Windows shutdown completely powered off the Switch Lite**, with the fan stopped. Wi-Fi, Bluetooth, audio and the gamepad were enabled; `LitePowerCheck.exe` and device isolation were not used for this successful run.

The tested combination was Windows 11 ARM64 23H2 (22631.2861), SwitchBattery **1.0.0.8**, Bluetooth **1.0.0.15**, SDR104 **0.4.0.1**, and R6 diagnostic BL31. The owner heard two diagnostic fan off/on cycles before power removal. Those pulses occur at the final B01 PSCI power-off boundary, before the PMIC operation, so this run reached the firmware handler.

The normal build now disables those pulses and uses **native PMC reset for Restart**. The native path requests the Hekate menu through Switchroot's RTC/BootROM preparation. The owner subsequently confirmed that **Windows Restart completed normally** with this native, no-pulse build. Shutdown and restart are separate firmware actions.

## Current sleep/wake work

The owner now prioritizes **S4 hibernation** (save RAM to SD, power off, then
restore the saved session). SC7 deep sleep is deferred. The 0.4.1.0 SD candidate
adds a bounded polled dump/hibernate path and separate small DMA allocation;
build and host tests pass, while physical S4 restoration remains pending. `winresume.efi`, `dumpsd.sys` and `crashdmp.sys` are
present in the installed trimmed ARM64 image.

Normal shutdown and restart acceptance above does not establish sleep support.
On 2026-11-07, the owner confirmed one fan-stop/display-wake cycle with fan
1.0.0.6, then reported lock-screen stalls and a black screen with a moving
cursor after repeated power-button presses. Windows recorded Modern Standby
transitions with zero DRIPS residency, followed by an unclean restart. No
application crash event identified a display-process fault.

The current candidate removes registry I/O from the fan display-notification
lock, drains power-button polling and RTC work before their I2C dependency
powers down, and adds optional PMIC wake interrupt resources. The paired
SDR104 driver saves and restores the quiesced controller's configuration.
Bluetooth also needs repair: the returned 1.0.0.15 log shows an uncompleted
patch download at record 150 followed by zero HCI events, while SafeMode and
BootPending were both zero. Version 1.0.0.17 fails that initialization honestly
and checks patch retention, reset acknowledgements and baud changes.

`SWITCH_LITE_HIBERNATE=1` exposes the Arm S4 object. With the matching SD
package, the one-time `Tools/Enable-LiteS4.cmd` enables a full hiberfile and maps
the power button to Hibernate after setup succeeds. It keeps Fast Startup off
and records a fixed result under `HKLM\SOFTWARE\SwitchLite\Hibernate`.
SC7 is outside this update. The first candidate failed on its initial cold
boot with `0xc000000f`, before this script ran; no hibernation image existed.
In a local read-only card replay with RAM-only writes, disabling only the
boot-start `tegra214pep` service allowed kernel startup. The S4 repair removes
that experimental package. This comparison isolates a loader blocker, not a
completed S4 resume or a full boot of the Lite under emulation.
After that package was removed and the paired BL33 installed, the owner
confirmed a normal desktop boot on the Lite.

The first S4 attempt powered off while running on battery. Its first following
boot hung black with the fan running; a second boot reached the desktop. The
owner had not set the process marker below. Readback confirms `SetupComplete=1`,
`LastError=0`, both button actions set to 2, and a 3.7 GiB full hiberfile.
Kernel-Boot event 16 records `0xc000007b`; event 158's diagnostic `0x40600017`
maps to header version/size/page-size/OS-version validation in the installed
22621.2792 resume loader, before restoration of memory. The header was zeroed
when the card returned, so its original contents cannot be inferred. S4 has
not passed. The owner confirmed there was no external power during that attempt.
The cause of the invalid header is not yet established.

The owner requested cleanup of the failed hiberfile and a hidden-FAT comparison.
The next setup recreates the same hiberfile; it does not create a new file on
every hibernation. S4 must work on battery. External power is only a comparison
condition, and the observed 54% capacity is not evidence of a battery fault.
The failed attempt also left BCD `AttemptResume=Yes`; older pre-hibernation
backups lacked that element. Cleanup removes that pending request together
with the invalidated file, retaining the loader's resume-object association.
This prevents a stale recovery request; it does not prove why the image failed.

The temporary Hekate option `hibernate_check=1` displays the first 64 bytes'
signature, version, header size, page size and OS major version before loading
Windows. It reads WINARM only, waits 12 seconds for a photograph, and continues
boot. It does not create a card log or alter the image. This captures evidence
before the resume loader or subsequent cold boot invalidates the header.

### Switchroot Ubuntu source comparison

The reference is Switchroot's `linux-5.1.2` kernel commit
`2d0059fd3167a8df756de2aa0489d4aa70a9fc15` and B01 board commit
`cf785c4c176499b301170d79fe57b77f365b73cd`.

| Path | Switchroot behavior | Windows port implication |
|---|---|---|
| [B01 SDMMC1](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/cf785c4c176499b301170d79fe57b77f365b73cd/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi#L280) | `power-off-rail`, tap 11, trim 14, SDR104 limit 200 MHz, dynamic host clock gating disabled, 8 MiB ADMA limit | Keep the board voltage/pad rules. The Windows 512 KiB runtime limit is a deliberate separate choice, not an Ubuntu equivalence claim. |
| [Host suspend/resume](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/host/sdhci-pltfm.c#L237) | Quiesce host/IRQs before the platform clocks; restore platform first, then host | Do not replay live command/DMA state or enable IRQs before initialization. |
| [Tegra resume](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/host/sdhci-tegra.c) | Mark pad calibration required and start at 400 kHz for identification | A saved SDR104 register image is insufficient after full card power loss. |
| [SD card resume](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/core/sd.c#L1789) | Power off after notification/deselection; power up and re-identify the same card | The Windows SDPORT bus-init path must re-establish card state; the retained-power context helper alone does not implement S4. |
| [ARM64 hibernate header](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/arch/arm64/kernel/hibernate.c#L148) | Validate saved-image invariants and resume CPU before restoring mappings/context | Investigate the current Windows header failure before treating it as a late device-resume failure. Windows still owns its hiberfile format and restore loader. |

For the S4 acceptance check, use a process-local marker in Command Prompt:
run `set S4TEST=123`, request hibernation, then cold-start Windows through
Hekate after complete power removal. In the original command window,
`echo %S4TEST%` must still print `123`. An application reopening its saved
tabs does not establish that the previous memory image was restored.

`SWITCH_LITE_SLEEP=1` exposes GSI 118 (PMU_EXT) as a wake interrupt and PMC
interrupt inversion. It no longer exposes the T214 PoFx processor backend.
That deferred backend requires `SWITCH_LITE_PROCESSOR_IDLE=1` explicitly and
must not be included in this S4 kit while its boot failure is unresolved.
The backend calls Windows `ProcessorHalt` for PSCI C7/CC6;
it does not invoke an unprepared, context-losing SMC. These changes compile
and pass focused host tests. **SC7 deep sleep, complete display restoration,
S4 hibernation and repeated physical wake are not yet accepted.** Progress
remains in [the porting goal](PORTING.md).

## Windows request ownership fix

The previous battery driver called `BatteryClassIoctl` on the underlying IRP of a KMDF-owned request. The battery class driver can complete or retain that WDM IRP directly, leaving KMDF's queue ownership unresolved during power-down.

SwitchBattery 1.0.0.8 calls the battery class driver from a WDM preprocess callback, before KMDF creates a request. Only unhandled IRPs are forwarded into KMDF. A separate lock protects class initialization, dispatch and unload without holding the I2C callback lock. This follows [Microsoft's battery sample](https://github.com/microsoft/Windows-driver-samples/blob/main/simbatt/func/wdf.c).

Earlier one-boot isolation of seven optional drivers allowed shutdown; restoring them brought the failure back. After the battery change, normal shutdown passed with that group enabled. Bluetooth 1.0.0.15's HCI write cancellation and frame-lifetime fix remains installed; it had not resolved the full failure on its own.

## Firmware behavior

R6 follows Switchroot's B01 `nvidia,bit-bang-after-shutdown` policy. Final power-off uses open-drain PY3/PY4 GPIO transactions to the MAX77620, preserving configuration bits and clearing reset-wake before requesting power removal. If execution continues after an attempt, pin ownership is restored and the hardware-I2C recovery path remains available.

Native restart stabilizes SCLK, prepares the PMC timings and complete BootROM command blocks, requests the Hekate menu through the RTC and then invokes the existing PMC system-reset path. It does not branch into the power-off handler.

Use [the pinned ATF recipe](../../patches/atf/README.md). `Tools/build-t214-atf.py` defaults to native restart and no fan trace. `--trace-power-boundary` enables the diagnostic pulses explicitly; `--restart-as-off` reproduces the earlier fallback build. These options are build-time diagnostics, not normal Hekate INI settings.

## Validation

- The actual battery dispatch function passed ASan/UBSan tests for immediate completion, pending requests, errors and unhandled fallback. ARM64 build, native catalog creation, signatures and installation readback passed.
- The PMIC simulation checks GPIO transactions, ACK/NACK handling, stuck-clock recovery, register preservation, BootROM checksums and the Hekate-menu request.
- The native BL31 builds at Hekate's `0xFFF00000` load address. Binary inspection confirms a reset call to `tegra_lite_prepare_reset` followed by `tegra_pmc_system_reset`, with no power-off branch or fan-trace function.
- Normal Windows shutdown has an owner-confirmed physical pass on one HDH-001. Native restart then passed on the normal build without fan pulses. These are physical observations, separate from the build and simulated checks.

## Primary references

- [Switchroot B01 PMC timings and board setup](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi)
- [B01 PMIC and shutdown GPIO policy](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-pmic.dtsi)
- [Linux PMC reset and BootROM preparation](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/soc/tegra/pmc.c)
- [RTC reboot reason](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/soc/tegra/pmc-r2p.c)
- [MAX77620 power-off](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/power/reset/max77620-poweroff.c)
- [Tegra I2C shutdown transfer selection](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/i2c/busses/i2c-tegra.c#L1949-L1967)
- [Nintendo NX ATF build parameters](https://github.com/LineageOS/android_device_nintendo_nx/blob/lineage-22.2/vendor/nx-vendor.mk)
