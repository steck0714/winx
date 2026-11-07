# HDH-001 shutdown and restart

The Home desktop has owner-confirmed audible output and working brightness controls. Returned-card readback confirms audio 1.0.0.9, fan 1.0.0.5 and a persistent 512 MiB pagefile with a 2048 MB maximum.

Power R3 reaches Windows. The owner tested restart only: the screen went black and the fan kept running. Shutdown on R3 is untested. Its returned card contains the expected BL31 and records the Windows restart request, user logoff, EventLog stop and ShutdownTime update. These show late shutdown progress, not execution of the final PSCI/PMIC handler.

## Switchroot B01 power route (R4)

The B01 NX device tree selects `maxim,system-pmic-power-off`, not `system-pmic-power-reset`. Its default reboot action is `pmc_r2p.action=bootloader`. Linux prepares the RTC reboot reason and BootROM commands before the existing ATF PMC reset. R3's direct PMIC restart attempt did not reproduce this path.

R4 keeps the official NX ATF build contract and changes the final B01 reset preparation:

- Configure the PMC CPU/system-clock requests with the NX active-high polarity and Linux's core good/off timer encoding.
- Publish the two complete, checksummed BootROM command blocks immediately before reset. The RTC block requests Hekate's menu. BootROM writes PMIC SD0 `0x16=0x20`, ONOFFCNFG2 `0x42=0x99` and ONOFFCNFG1 `0x41=0x80` after the SoC reset. The running ARM CPU does not directly drop its core rail.
- Retain ATF SCLK stabilization, clear stale recovery/RCM flags and continue through the existing PMC reset path.
- For shutdown, use the MAX77620 driver's interrupt-latch reads and read-modify-write policy: clear SFT_RST_WK, then set SFT_RST while preserving the remaining configuration bits.

The existing UEFI BootROM setup remains compatible. R4 refreshes the table at the final reset action and sets the Switchroot Hekate-menu reason. It replaces only `switchroot/woa-lite-home/bl31.bin`; the entry remains **Windows 11 Lite**.

R4 was built and deployed with a readback match. The owner reached Windows and tested restart: it still leaves a black screen with the fan running. R4 shutdown also fails with the fan running; the owner reports a recognizable Windows sound after the screen goes black. Matching Switchroot's sequence did not resolve the observed restart failure; there is still no direct evidence that Windows reaches the final PSCI handler. Further late-shutdown/firmware-boundary diagnosis remains open while Wi-Fi development continues.

## Pre-Windows boundary check

PowerCheck R2 uses a Hekate BL33 environment setting instead of a held volume
button. Hekate intercepts volume-minus itself, so the previous immediate return
to Hekate did not execute the firmware check and is not a reset result.

With `power_check=2`, the Home boot manager displays
`FIRMWARE POWER-OFF CHECK / CONFIG 2`, counts down five seconds and calls
`ResetSystem(EfiResetShutdown)` before loading Windows Boot Manager. The owner
confirmed normal automatic shutdown with the same R4 BL31. This establishes
pre-Windows power-off, not Windows shutdown. Windows may stall before the final
firmware call or leave hardware in a different state; the failing boundary has
not yet been identified.

For an explicit diagnostic run, `power_check=1` selects pre-Windows cold reset;
that action remains untested. Missing/zero selects the normal Windows path.
These options require the B01 Home touch/performance profile and its reserved
Hekate environment. The temporary `power_check=2` line was removed from the
physical card after the successful test, restoring normal Home launch.

## Windows power-off fallback (R5)

On the returned Windows 11 23H2 card, User32 recorded the requested shutdown (1074) at 00:37:26 UTC and EventLog stopped (6006) at 00:37:34. The retained real-time event log contains Kernel-Power 109 at 00:37:37 and Kernel-General 13 at 00:37:39. Windows continued its shutdown sequence after the event service stopped. Hibernation and Fast Startup were disabled. The logs do not record entry to the final firmware call.

Inspection of this installation's ARM64 kernel with its matching Microsoft public symbols shows that `HalpShutdown` can issue PSCI `SYSTEM_OFF` (0x84000008), then try UEFI shutdown if control returns. On the boot CPU, `HalpPowerWriteResetCommand` can issue `SYSTEM_RESET` (0x84000009) before its UEFI reset fallback. The FADT selects PSCI through SMC. These are static call paths. The last instruction reached on the failing console is still unknown.

R4 required both interrupt-latch reads to succeed before asking the MAX77620 to turn off. Switchroot Ubuntu treats those reads as best effort. R5 follows that behavior, restores the system clock before the PMIC transaction, and checks the wake configuration before the final power cut. If the CPU is still running after the first attempt, it resets I²C5 and tries once more with Switchroot's nine-clock, STOP and bus-clear completion sequence. [Switchroot PMIC power-off](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/power/reset/max77620-poweroff.c) · [Tegra I²C bus recovery](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/i2c/busses/i2c-tegra.c)

The owner tested **Shut down** with the installed R5 firmware. The screen went black, the fan continued running and sped up after about ten seconds, and a long power-button press was needed. The last recorded sample was 42 °C with fan duty 51/236 and zero sensor failures. The fan driver's suspend callback sets duty to 76/236 after restoring the CPU clock. That is consistent with the sound, but the final duty and entry to PSCI are unknown.

The Windows-specific R5 BL31 routes **Restart** to the same full power-off handler on B01. The ARM64 binary contains that branch and simulated I²C error recovery passed. Restart on the installed R5 build still awaits a separate hardware result.

## GPIO power-off path (R6)

Kernel-Power 109 on the returned card carries action 6 (`PowerActionShutdownOff`). At 00:37:40.196 UTC, the boot status file recorded a power transition with `SystemRunning` cleared. Its reference time has a valid CRC and follows the last Kernel-General event. The matching Windows kernel clears this bit in `PopSetCleanShutdownMarker`, before its final filesystem, device-power and HAL calls. This places the failure late in shutdown. It still leaves open whether Windows reached BL31. [Microsoft boot status data](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/-blackboxbsd)

The Switchroot B01 board enables `nvidia,bit-bang-after-shutdown` on the PMIC bus. When its I²C adapter is shutting down or restricted to atomic transfers, the Ubuntu driver uses GPIO. R5 retried the hardware controller on both attempts. R6 implements the board's GPIO route in the final B01 PSCI power-off handler, which also services restart in the Windows build. [B01 PMIC device tree](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-pmic.dtsi) · [Switchroot I²C transfer selection](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/i2c/busses/i2c-tegra.c#L1949-L1967)

R6 uses open-drain PY3 (clock) and PY4 (data). It reads back the wake configuration before requesting power-off and restores pin ownership if execution continues. The existing controller recovery path then remains available. An independent I²C slave model decoded the transactions, repeated STARTs, ACKs and final NACKs and exercised a stuck clock, address NACK and failed configuration readback. The ARM64 firmware was built. The test Lite's card still has R5, and physical R6 shutdown and restart are pending.

## Build and evidence

Use [the pinned source and build instructions](../../patches/atf/README.md). The Nintendo NX recipe requires `TZDRAM_BASE=0xFFF00000`, reset-to-BL31 startup and the IRAM handoff. R2 used incorrect defaults and failed before Windows; it was withdrawn. R3 corrected that boot regression. R4 retains the same entrypoint and flags.

The focused simulated-MMIO test covers decoded BootROM commands/checksums, Hekate-menu encoding, PMC request/timer values, and shutdown register preservation/failure paths. The AARCH64 firmware builds successfully. This does not substitute for physical reset/power-off acceptance.

## Primary references

- [Switchroot B01 NX PMC timings, BootROM commands and boot arguments](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi)
- [B01 NX PMIC power-off selection](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-pmic.dtsi)
- [Linux PMC initialization, BootROM encoding and restart](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/soc/tegra/pmc.c)
- [B01 RTC reboot reason](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/soc/tegra/pmc-r2p.c)
- [MAX77620 shutdown](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/power/reset/max77620-poweroff.c)
- [Switchroot ATF final PSCI actions](https://github.com/CTCaer/switch-l4t-atf/blob/switch-v5-atf2.6/plat/nvidia/tegra/soc/t210/plat_psci_handlers.c)
- [Nintendo NX ATF build parameters](https://github.com/LineageOS/android_device_nintendo_nx/blob/lineage-22.2/vendor/nx-vendor.mk)
