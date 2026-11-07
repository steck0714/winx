# HDH-001 shutdown and restart

## Current result

On 2026-11-06, the owner confirmed that **normal Windows shutdown completely powered off the Switch Lite**, with the fan stopped. Wi-Fi, Bluetooth, audio and the gamepad were enabled; `LitePowerCheck.exe` and device isolation were not used for this successful run.

The tested combination was Windows 11 ARM64 23H2 (22631.2861), SwitchBattery **1.0.0.8**, Bluetooth **1.0.0.15**, SDR104 **0.4.0.1**, and R6 diagnostic BL31. The owner heard two diagnostic fan off/on cycles before power removal. Those pulses occur at the final B01 PSCI power-off boundary, before the PMIC operation, so this run reached the firmware handler.

The normal build now disables those pulses and uses **native PMC reset for Restart**. The native path requests the Hekate menu through Switchroot's RTC/BootROM preparation. The owner subsequently confirmed that **Windows Restart completed normally** with this native, no-pulse build. Shutdown and restart are separate firmware actions.

## Current sleep/wake work

The goal is **retained-memory sleep and direct power-button wake without
Hekate**. Tegra SC7 is a platform sleep state, not an ACPI "S7" state. Normal
shutdown, display-off and CPU idle are not acceptance of whole-system sleep.
SC7 and reliable repeated wake are **not implemented/accepted yet**. The owner
cancelled S4; its ACPI object, SD dump path, setup script and header probe have
been removed. S4 is no longer part of the plan.

On 2026-11-07, fan 1.0.0.6 passed one display-off/on cycle, but repeated
power-button presses later stalled the lock screen or left only a moving
cursor. Windows recorded Modern Standby transitions with zero DRIPS residency.
Fan 1.0.0.7 removes registry I/O from the display-notification lock. The current
power-button and RTC candidates drain polling/work before I2C powers down;
SIO 0.2.4 and touch 1.0.1.1 use asynchronous display-driven runtime idle.
Bluetooth 1.0.0.18 retains strict patch/baud validation, removes the persistent
failed-boot hold and permits one T214 hardware-reset retry. It is installed
with DDR200 0.5.0.1; physical discovery and lifecycle results remain pending.

`SWITCH_LITE_SLEEP=1` exposes PMU_EXT GSI 118 as a wake interrupt and configures
PMC interrupt inversion. `SWITCH_LITE_PROCESSOR_IDLE=1` separately exposes the
experimental T214 PEP. Its boot-start configuration caused `0xc000000f`; the
owner reached the desktop after it was removed. Resolve that loader failure
before deployment. Calling `ProcessorHalt` for C7/CC6 alone does not provide
SC7 firmware context, peripheral sequencing or a valid wake path.

The 0.1.0.2 PEP candidate now uses a native WDM entry/PnP lifecycle, following
Microsoft's i.MX PEP, and imports no KMDF loader. It defers registry diagnostics
until device start and drains display callbacks/work on removal. ARM64 and
focused callback tests pass; the old loader failure has not been retested on
hardware and the candidate is not installed.

BL31 development patch 0005 now prepares BPMP and PMC wake event 51 for the
Windows `CPU_SUSPEND` SC7 path. It rejects missing acknowledgement or failed
wake-mask setup before committing to powerdown. Its default is disabled;
`--sc7` builds the candidate separately. ARM64 builds and actual-function
fault tests pass, but it is not installed. GPIO 1.0.0.5 now saves/restores
controller, debounce and interrupt state through GpioClx, with output latches
restored before output enable and interrupts last. It preserves pending wake
events and performs no registry I/O in power callbacks. The ARM64 package is
signed and its register-loss tests pass; physical restoration remains pending.
Windows device constraints, display/clock restoration and complete SC7
coordination are still required before enabling this path on the card.

### Switchroot Ubuntu reference

Use kernel commit `2d0059fd3167a8df756de2aa0489d4aa70a9fc15` and B01 board
commit `cf785c4c176499b301170d79fe57b77f365b73cd` as pinned references.
The companion NVIDIA kernel tree for this release is tag `linux-5.1.0`, commit
`578d533b99cd5d5c3e57760431d2f621307265cf` (not a `linux-5.1.2` branch).

| Path | Switchroot behavior | Windows port implication |
|---|---|---|
| [B01 boot preparation](https://github.com/CTCaer/hekate/blob/v6.5.3/bootloader/l4t/l4t.c#L978) | Hekate loads BPMP/MTC from `bootloader/sys/l4t` and configures B01 warmboot | Reuse this boot preparation; B01 does not use the original T210's separate `sc7entry.bin`. |
| [SC7 permission](https://github.com/CTCaer/switch-l4t-kernel-nvidia/blob/578d533b99cd5d5c3e57760431d2f621307265cf/drivers/soc/tegra/pm-tegra210.c#L68) | Send BPMP `MRQ_ENABLE_SUSPEND` with `TEGRA_PM_SC7` before system suspend | Implemented in opt-in BL31 patch 0005; caller/device coordination and physical acceptance remain pending. |
| [PMIC wake routing](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/cf785c4c176499b301170d79fe57b77f365b73cd/kernel-dts/nx-platforms/tegra210b01-nx-pmic.dtsi#L110) | MAX77620 uses active-low PMC wake event 51, separately from GIC PMU_EXT | Patch 0005 arms and checks this source after BPMP acknowledgement. GIC configuration alone is insufficient for SC7. |
| [GPIO restore](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/gpio/gpio-tegra.c#L490) | Save CNF, output/direction, debounce and interrupt state | GPIO 1.0.0.5 implements GpioClx save/restore with wake-status preservation; signed, not installed. |
| [B01 SDMMC1](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/cf785c4c176499b301170d79fe57b77f365b73cd/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi#L280) | `power-off-rail`, tap 11, trim 14, SDR104 limit 200 MHz, dynamic host clock gating disabled | Preserve voltage/pad rules and coordinate the real card power state. |
| [Host suspend/resume](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/host/sdhci-pltfm.c#L237) | Quiesce host/IRQs before platform clocks; restore platform first | Do not replay live command/DMA state or enable IRQs before initialization. |
| [Tegra resume](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/host/sdhci-tegra.c) | Mark pad calibration required and start at 400 kHz | A saved SDR104 register image is insufficient after full card power loss. |
| [Card resume](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/mmc/core/sd.c#L1789) | Power up and re-identify the same card | Windows must re-establish card state when power was lost; the retained-power context helper alone is insufficient. |

Track the complete acceptance and DDR200 work in [the porting goal](PORTING.md).

## Windows request ownership fix

The previous battery driver called `BatteryClassIoctl` on the underlying IRP of a KMDF-owned request. The battery class driver can complete or retain that WDM IRP directly, leaving KMDF's queue ownership unresolved during power-down.

SwitchBattery 1.0.0.8 calls the battery class driver from a WDM preprocess callback, before KMDF creates a request. Only unhandled IRPs are forwarded into KMDF. A separate lock protects class initialization, dispatch and unload without holding the I2C callback lock. This follows [Microsoft's battery sample](https://github.com/microsoft/Windows-driver-samples/blob/main/simbatt/func/wdf.c).

Earlier one-boot isolation of seven optional drivers allowed shutdown; restoring them brought the failure back. After the battery change, normal shutdown passed with that group enabled. Bluetooth 1.0.0.15's HCI write cancellation and frame-lifetime fix was present in that test; it had not resolved the full failure on its own. The current 1.0.0.18 package retains that fix.

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
