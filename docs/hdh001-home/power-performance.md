# HDH-001 shutdown and restart

## Current result

On 2026-11-06, the owner confirmed that **normal Windows shutdown completely powered off the Switch Lite**, with the fan stopped. Wi-Fi, Bluetooth, audio and the gamepad were enabled; `LitePowerCheck.exe` and device isolation were not used for this successful run.

The tested combination was Windows 11 ARM64 23H2 (22631.2861), SwitchBattery **1.0.0.8**, Bluetooth **1.0.0.15**, SDR104 **0.4.0.1**, and R6 diagnostic BL31. The owner heard two diagnostic fan off/on cycles before power removal. Those pulses occur at the final B01 PSCI power-off boundary, before the PMIC operation, so this run reached the firmware handler.

The native PMC restart build passed with fan tracing disabled. On 2026-11-09, **BL31 r8** returned a remotely restarted Lite to Windows with Hekate autoboot disabled. The owner confirmed that Windows Restart goes directly to Windows and powering on after Shutdown opens Hekate. R8 remembers the entry Hekate used for the current boot and requests that entry on Restart.

## Current sleep/wake work

On 2026-11-08, the owner confirmed that **switchpwrbtn 1.0.0.4** restores the display and normal input after repeated short power-button screen-off/wake cycles. Runtime counters recorded **12 Power Down reports and 12 Wake Up reports**; Windows recorded paired Modern Standby entry/exit events and SSH remained reachable after wake.

This is the tested **S0 Modern Standby screen-off/wake** path. It is not SC7 deep sleep or S4 hibernation. Long-duration idle and the Windows sleep-menu entry path are not established by this short-button test. The tested [1.0.0.4 source](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/tree/hdh001-winarm-bounded-write/drivers/SwitchPowerButton) is included in the default driver branch and the paired firmware submodule. Install the rebuilt 1.0.0.4 package to use the correction.

A later long screen-off session on battery became unresponsive and required a forced power-off. Windows recorded a Power Button standby exit after about 43 minutes, followed by an unexpected restart. The failing component is not identified; long-duration standby remains unresolved.

The driver selects HID Wake Up when the display is off and Power Down while
it is on. Holding a wake press across the display transition does not submit
another power request. Polling lifecycle is drained/restarted without adding
PMIC/GIC wake interrupts or a processor PEP. SC7 development remains paused on
[`sc7-test`](https://github.com/ruje0504-2/winx/tree/sc7-test), with the matching
driver branch; S4 remains removed. Normal shutdown and native restart retain
their separate accepted results above.

Bluetooth 1.0.0.19 retains bounded patch/reset recovery and UART-D host
preparation. SD 0.5.0.3 retains DDR200 development and corrects clock ordering
when the card keeps power. These ordinary device-recovery changes remain in
the default branch and do not require the experimental PEP. Their builds,
focused tests and package signatures passed. SD 0.5.0.3 additionally fixes
event-mask loss during private SDR104 fallback; it and Bluetooth 1.0.0.19
are installed/readback-verified. The owner confirmed desktop boot and
working Bluetooth; actual DDR200 selection/fallback remain unverified.
The earlier Bluetooth 1.0.0.18 + DDR200 0.5.0.1 combination failed with
INACCESSIBLE_BOOT_DEVICE. The owner supplied the stop-code photograph; no
current crash dump or driver diagnostics survived on the returned card.
The cause remains under investigation. [Current status](PORTING.md)

## Charge termination

SwitchBattery **1.0.0.11** enables BQ24193 hardware termination and an eight-hour charge safety timer independently of the optional USB-C PD connection. It preserves the existing voltage/current limits, temperature settings and charge/OTG mode. Normal termination uses the configured voltage/current thresholds; a voltage drop below the recharge threshold starts a new cycle without waiting eight hours. The driver does not repeatedly reset the safety timer or stop charging solely because the gauge displays 100%.

On 2026-11-08, the ARM64 build, native SYS/catalog signatures and live installation passed. The running driver reported version 11, successful REG05 readback **0x02 → 0x8A**, and a started battery device charging at about 91%. A complete full-charge/recharge cycle has not yet been observed. Earlier full-percentage readings did not show voltage exceeding the configured 4.192 V limit. Fixed initialization results use the existing driver Parameters key; the temporary monitoring package was removed.

[Implementation, tests and Switchroot comparison](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/docs/hdh001/README.md#charge-termination) · [TI BQ24193 charging behavior](https://www.ti.com/lit/ds/symlink/bq24193.pdf)

## Windows request ownership fix

The previous battery driver called `BatteryClassIoctl` on the underlying IRP of a KMDF-owned request. The battery class driver can complete or retain that WDM IRP directly, leaving KMDF's queue ownership unresolved during power-down.

SwitchBattery 1.0.0.8 calls the battery class driver from a WDM preprocess callback, before KMDF creates a request. Only unhandled IRPs are forwarded into KMDF. A separate lock protects class initialization, dispatch and unload without holding the I2C callback lock. This follows [Microsoft's battery sample](https://github.com/microsoft/Windows-driver-samples/blob/main/simbatt/func/wdf.c).

Earlier one-boot isolation of seven optional drivers allowed shutdown; restoring them brought the failure back. After the battery change, normal shutdown passed with that group enabled. Bluetooth 1.0.0.15's HCI write cancellation and frame-lifetime fix was present in that test; it had not resolved the full failure on its own. The current 1.0.0.18 package retains that fix.

## Firmware behavior

R6 follows Switchroot's B01 `nvidia,bit-bang-after-shutdown` policy. Final power-off uses open-drain PY3/PY4 GPIO transactions to the MAX77620, preserving configuration bits and clearing reset-wake before requesting power removal. If execution continues after an attempt, pin ownership is restored and the hardware-I2C recovery path remains available.

Native restart stabilizes SCLK, prepares PMC timings and complete BootROM command blocks, then invokes the PMC system reset. R8 writes the saved entry selector through the RTC block. The menu is the fallback when the bootloader handoff has no valid selector.

Use [the pinned ATF recipe](../../patches/atf/README.md). `Tools/build-t214-atf.py` defaults to native restart and no fan trace. `--trace-power-boundary` enables the diagnostic pulses explicitly; `--restart-as-off` reproduces the earlier fallback build. These options are build-time diagnostics, not normal Hekate INI settings.

## Validation

- The actual battery dispatch function passed ASan/UBSan tests for immediate completion, pending requests, errors and unhandled fallback. ARM64 build, native catalog creation, signatures and installation readback passed.
- The PMIC simulation checks GPIO transactions, ACK/NACK handling, stuck-clock recovery, register preservation, BootROM checksums and the Hekate restart selector.
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
