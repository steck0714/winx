# HDH-001 shutdown and restart

## Current result

On 2026-11-06, the owner confirmed that **normal Windows shutdown completely powered off the Switch Lite**, with the fan stopped. Wi-Fi, Bluetooth, audio and the gamepad were enabled; `LitePowerCheck.exe` and device isolation were not used for this successful run.

The tested combination was Windows 11 ARM64 23H2 (22631.2861), SwitchBattery **1.0.0.8**, Bluetooth **1.0.0.15**, SDR104 **0.4.0.1**, and R6 diagnostic BL31. The owner heard two diagnostic fan off/on cycles before power removal. Those pulses occur at the final B01 PSCI power-off boundary, before the PMIC operation, so this run reached the firmware handler.

The native PMC restart build passed with fan tracing disabled. On 2026-11-09, **BL31 r8** returned a remotely restarted Lite to Windows with Hekate autoboot disabled. The owner confirmed that Windows Restart goes directly to Windows and powering on after Shutdown opens Hekate. R8 remembers the entry Hekate used for the current boot and requests that entry on Restart.

## Current DVFS integration — 2026-11-15

| Component | Current work / evidence |
|---|---|
| CPU/GPU | Seven Noble profiles, independent saved user choice; Handheld first-use/reset default. GPU source remains in mesa-switch-woa |
| RAM boot | Same-machine Noble request2500000kHz, VDD2=1175mV and VDDQ=640mV; real2500000kHz and four trained points read on Windows |
| Runtime EMC | Fan33: Noble ACTMON IRQ plus CPU/display bandwidth floors;204000 /1331200 /1600000 /2500000kHz. Native real IRQ, completed transitions and errors0 verified; owner manual testing passed |
| Firmware contract | BPMP owns three base timings plus one trained OC slot. WinX records exact rounded/readback rate and checks VDD_CORE; FAN0 ACPI supplies ACTMON GSIV77. No invented intermediate timings |
| Desktop tools | Switch Power saves power/fan independently; Pacecar adds battery net power and50% background transparency |
| Display | Current .158 + bit14 + fixed UMD/ICD, bidirectional fence and activation5s after logon retained; SC7 disabled |

The first high-RAM installation was restored because the installation script
required the GPU voltage ACK before enabling the gated GPU. The corrected
installation-stage predicate accepts only the observed fresh, unchanged,
fault-free2500MHz boot state with adequate read-back GPU voltage. Fan33 runtime
readiness was verified after boot16:11:46UTC, and the owner subsequently
reported completed manual testing and success. Workload duration and individual
frequency coverage were not specified. The exact Fan32 preceding pairing is
retained for recovery. Full results and hashes are maintained in
[the matched-driver record](https://github.com/ruje0504-2/mesa-switch-woa/blob/tgd3d-vulkan/tegra-x1-wddm/docs/DVFS_REVIEWED_20261114.md).

The2500MHz entry is this device's captured Noble configuration, not a universal
frequency guarantee for all RAM chips. Support comes from actual BPMP training.
CPU frequency and voltages are still selected from the chip's fuse-derived
DFLL tables; the boot `cpu_oc=1996800` handoff remains unchanged.

## Historical November13 fixed-frequency selection



On November 13 the owner selected a lower-frequency GPU/EMC combination and
kept CPU settings unchanged. The WinX configuration change is limited to
`ram_oc=1600000` in `woa-lite-home-performance.ini`; the existing firmware
already supports that request. `cpu_oc=1996800` remains the boot handoff
request, while SwitchFan 13 continues to select 1497.6 MHz in Windows.

| Component | Selected setting | Readback / status |
|---|---|---|
| CPU | 1497.6 MHz, existing voltage | 1497600 kHz / 1120 mV after restart |
| RAM | 1600 MHz, existing voltage | Requested and actual 1600000 kHz; handoff Ready (1) |
| GPU | 460.8 MHz, 800 mV fixed-profile floor | Hardware counter 460800 kHz; startup PMIC setting readback 800000 µV |
| Fan | SwitchFan 1.0.0.13 | Version 13 retained |

Previously RAM requested 2100 MHz but actually ran at 1866 MHz with
RateMismatch (8). The new 1600 MHz result was read after the restart requested
at 2026-11-14 03:10:27 UTC. The owner confirmed an operable Windows desktop
while the GPU remained behind its startup gate. Native desktop stability and
subsequent direct tgd3d cold boot are separate owner-run tests. The later
direct cold boot still rebooted automatically and is recorded as failed.
The paired GPU package was then installed and enabled without another OS
restart. At 03:16:00 UTC it reported Code 0 and DWM loaded tgd3d; this verifies
installation and clock selection, not the pending physical stability tests.
At 03:24:35 UTC the owner requested direct native tgd3d boot again. The same
GPU package was armed persistently and the logon activation task disabled;
CPU, GPU and RAM readbacks were unchanged. No restart was requested by the
controller. The owner subsequently reported another automatic reboot.
The owner reports somewhat better weather/browser stability on external
power than battery, with occasional reboots even while plugged in. There
is no controlled equal-load/equal-duration AC/DC comparison or established
root cause. Lowering GPU/EMC did not resolve direct native cold boot.

The offline update restored the configuration for activation after interactive logon
(PT5S task), preserving the clocks above. At the owner's additional request,
the paired UMD enables the existing bidirectional GPU Fence with the
reviewed Flush error-reporting fix; ARM64/x64 builds and x64 CPU controls
pass. SD readback verified 74 files and the scoped registry changes;
Windows logon activation and physical acceptance await the owner. KMD and ICD are unchanged.
A CPU-sync logon recovery archive is retained. No new SSH test or agent
delegation accompanies this operation; wait for the owner to arrange Claude.
GPU code and detailed results remain in
[mesa-switch-woa](https://github.com/ruje0504-2/mesa-switch-woa/blob/tgd3d-vulkan/tegra-x1-wddm/docs/TGD3D_POWER_SPEEDO_20261113.md).
No battery, charging, SD, CPU voltage or sleep-policy code changes accompany
this WinX configuration update.

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
preparation. The historical SD 0.5.0.3 update retained DDR200 development and corrected clock ordering
when the card keeps power. These ordinary device-recovery changes remain in
the default branch and do not require the experimental PEP. Their builds,
focused tests and package signatures passed. SD 0.5.0.3 additionally fixes
event-mask loss during private SDR104 fallback. The owner confirmed desktop
boot and working Bluetooth on that combination. The current SD version is
**0.5.1.2**, verified after the November 12 17:48 PST restart
(November 13 01:48 UTC) with an 8 MiB write/flush/readback check. It advertises
capability but records isolated passing taps 30 and 77 in 128 DDR200 samples,
with zero timeouts and a one-tap maximum window, then successful SDR104 fallback.
Crash-dump takeover support does not enable S4. The full power-settings
snapshot was unchanged. The owner-requested CPU default was updated to
**1497.6 MHz** by **SwitchFan 1.0.0.13**; requested and actual clock readbacks
matched after the same restart. The remaining fan and RAM settings were
unchanged. These storage/clock checks do not add sleep/wake acceptance.
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
