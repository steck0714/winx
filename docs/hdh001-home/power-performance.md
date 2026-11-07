# HDH-001 shutdown and restart

## Current result

On 2026-11-06, the owner confirmed that **normal Windows shutdown completely powered off the Switch Lite**, with the fan stopped. Wi-Fi, Bluetooth, audio and the gamepad were enabled; `LitePowerCheck.exe` and device isolation were not used for this successful run.

The tested combination was Windows 11 ARM64 23H2 (22631.2861), SwitchBattery **1.0.0.8**, Bluetooth **1.0.0.15**, SDR104 **0.4.0.1**, and R6 diagnostic BL31. The owner heard two diagnostic fan off/on cycles before power removal. Those pulses occur at the final B01 PSCI power-off boundary, before the PMIC operation, so this run reached the firmware handler.

The normal build now disables those pulses and uses **native PMC reset for Restart**. The native path requests the Hekate menu through Switchroot's RTC/BootROM preparation. The owner subsequently confirmed that **Windows Restart completed normally** with this native, no-pulse build. Shutdown and restart are separate firmware actions.

## Current sleep/wake work

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

`SWITCH_LITE_SLEEP=1` is an experimental firmware profile. It exposes GSI 118
(PMU_EXT) as a wake interrupt, PMC interrupt inversion, and the T214 PoFx
processor backend. The backend calls Windows `ProcessorHalt` for PSCI C7/CC6;
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
