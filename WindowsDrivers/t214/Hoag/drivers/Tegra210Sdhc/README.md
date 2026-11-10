# Tegra210Sdhc (nvtsdhc)

SD host miniport for `sdport.sys`, for the Switch's microSD slot (Tegra210 SDMMC1, `ACPI\NVDA0101`).

- Based on the SDHC miniport in Microsoft's Windows-driver-samples (`sd/miniport/sdhc`, MIT license).
- Tegra210 limits: 3.3 V, legacy/high speed only (`NV_T210_SAFE_BRINGUP`), no UHS-I voltage switch or tuning.
- `NV_T210_PIO_ONLY` (default off): all data goes through the data port, never DMA. On Windows 11 22621 the
  inbox sdbus could not mount the boot volume on this non-coherent controller (`_CCA = 0`).
- Build: `tools\Build-Driver.ps1 -Wdm -Name nvtsdhc -Sources drivers\Tegra210Sdhc\src\nvtsdhc.c -Libs sdport.lib -Inf drivers\Tegra210Sdhc\nvtsdhc.inf -OutDir build\Tegra210Sdhc`

The UEFI does not describe the SD host yet. `loader\sd-acpi-chainloader.zig` (built as `SDLOADAA64.EFI`,
placed at `EFI\Boot\bootaa64.efi`) installs `acpi\SDH0-Test.asl` as an SSDT and then starts Windows Boot Manager.

Boot diagnostics (`NV_DIAG_STOP`, default on while bringing it up): if fewer than 8 blocks have been read
2.5 s after `DriverEntry`, the driver bugchecks with stop code `0xDSFCCEEK` (stage, flags, last command,
last error command, error kind; see the comment at the top of `src/nvtsdhc.c`). Card detect always reports
a card, since the Switch wires it to a GPIO.

## Switch Lite (HDH-001) read test

For a RAM-booted WinPE that only needs to read its deployment card, add
`-Defines NV_T210_LITE_READ_ONLY=1` to the PowerShell build command above and use a separate output directory.
Use firmware that exposes the external slot as `ACPI\NVDA0101` with `_CCA = 0`, leaves it at 3.3 V
and selects PLLP for SDMMC1. With WinX's SDM1 already present, load the boot manager directly;
adding the example SSDT would describe the same controller twice. Inject the resulting signed package
into the ARM64 WinPE image before boot.

The profile reads the firmware's CAR divider, uses 1- or 4-bit default-speed PIO up to 25 MHz,
and reports write protection to SDPORT. Data-out, block writes, write-protect changes and erase
are rejected before a request is queued. The diagnostic stop timer and crash-dump path are disabled.
For an on-device read check, place a marker file on the external FAT volume, boot to the WinPE RAM
disk, and read that file from the external volume. One HDH-001 running 22621.1 passed this check
with the development build. The default ADMA configuration was outside that test.

## Restricted WINARM writes on HDH-001

Build with `NV_T210_LITE_WINARM_WRITE=1`. The default card profile presents the 80 GiB NTFS partition `WINARM` as the sole, active MBR entry. Windows accepts block writes only inside that partition after checking its identity. Hekate still reads the physical FAT32 volume.

To also mount FAT32 in Windows, pass `--expose-fat32` to the generator. This experimental profile projects FAT32 as the second MBR entry and permits writes there only after validating its extent and boot sector. Earlier trials reported stalls, but the latest owner comparison found similar responsiveness with FAT32 visible or hidden. No reproducible performance difference or root cause is established; the current card uses the visible profile.

The generator reads a saved whole-card image and validates its hybrid MBR, primary GPT and NTFS boot sector. The tested layout places WINARM in MBR slot 4 and GPT slot 11. Both profiles protect the physical partition tables and other partitions from writes. Keep the generated identifiers private.

Build BL33 and the SD miniport from **the same generated header**, and deploy them as a pair. Mixing a one-volume firmware with a two-volume miniport caused `INACCESSIBLE_BOOT_DEVICE` (0x7B). The card comparison used miniport 0.3.2.5 (WINARM only) and 0.3.2.6 (WINARM + FAT32). Source version 0.3.2.7 supports both profiles.

From the repository root in PowerShell:

```powershell
$sd = 'drivers/Tegra210Sdhc'
py -3 "$sd/winarm/configure-target.py" 'D:/images/test-card.img' "$sd/winarm/generated"
Copy-Item "$sd/winarm/generated/winarm-target.h" "$sd/src/"
./tools/Build-Driver.ps1 -Wdm -Name nvtsdhc -Sources "$sd/src/nvtsdhc.c" -Libs sdport.lib -Defines 'NV_T210_LITE_WINARM_WRITE=1' -Inf "$sd/winarm/nvtsdhc.inf" -OutDir build/WINARM-R3
```

For WinPE deployment, use the matching R3 BL33, generated startnet.cmd and the signed test package. Verify the final SYS and INF against their catalog using a test certificate valid at the firmware's cold-start clock. The hardware build and test steps are in the [R3 source tree](https://github.com/ruje0504-2/winx/tree/b44da7475fb281d6e4390d976f8f8e0098154d87/docs/hdh001-winarm-r3). One HDH-001 verified the 4 KiB receipt file after shutdown and a second boot. [Hardware report](https://github.com/steck0714/winx/issues/5)

### T214 SDR104 candidate

The `NV_T214_SDR104=1` bounded WINARM profile adds B01 SDMMC1 signaling
voltage switching, pad calibration, hardware CMD19 tuning and a 199.68 MHz
PLLC4 OUT2 clock. SDR50, DDR50 and proprietary DDR200 are not advertised.
It requires the accompanying `tegra210i2c` 1.0.0.5 package: both drivers
serialize I2C5 access while the boot miniport adjusts the SD LDO2 rail.

Large requests can use a private noncached, below-4-GiB ADMA2 buffer. The
existing MBR presentation and per-card write bounds are applied before
completion; the default WINARM-only view is unchanged. Allocation or mapping
constraints keep PIO available. Controller errors are returned to SDPORT
and a failed DMA operation requires a completed DAT reset before reuse.

Hardware result for 0.4.0.1: Windows reached the desktop and the returned
SYSTEM hive reports `BusSpeed=7` (SDR104), `Signaling18V=1`, `BusWidth=4`,
`ClockKhzActual=199680`, `Tuned=1`, `TuneStatus=0`, 128 tuning iterations
and tap 64. ADMA2 completed 1,867,466 blocks with `AdmaErrors=0`; voltage
switching and pad calibration recorded no failures or fallbacks. These are
driver/host readbacks from the observed run, not a standalone throughput
benchmark or extended integrity/stress qualification.

ARM64 builds and host DMA/filter/write-bound tests pass. Read `Signaling18V`,
`Tuned`, `TuningTap`, `ClockKhzActual`, `AdmaBlocks` and `AdmaErrors` under
`Services\nvtsdhc\Parameters\Diag`.

Hardware references: [Hekate SDMMC](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/storage/sdmmc_driver.c),
[Hekate clock setup](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/soc/clock.c),
and [Switchroot Linux](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-dev/drivers/mmc/host/sdhci-tegra.c).

Version 0.4.0.1 repairs a reset-all power-state mismatch: the card's external
rail may remain on while SDHCI `POWER_CONTROL` has reset to zero. A subsequent
3.3 V request now restores and checks the controller power bits instead of
returning early. The regression reproduces that cleared-register state and
also checks a real power cycle when leaving 1.8 V. This fix passes the host
regression and ARM64 build; the owner subsequently reached Windows with 0.4.0.1. Its signaling, tuning
and ADMA readbacks are recorded above; sustained throughput is still unmeasured.

### FAT32-visible and resume candidate (0.4.0.3)

The owner requested another FAT32-visible trial using SDR104/ADMA2, preserving
the hidden profile. Both builds share the same validated write bounds. DMA
read completion now dispatches identity checks only for the few relevant
metadata sectors, rather than once per returned sector. Tests compare the
result with the previous sector-by-sector path across 1–128-block reads in
both partition layouts. The card's FAT32 FSInfo signatures and free-count
field were valid; no FSInfo metadata is rewritten or fabricated.

Version 0.4.0.3 also removes synchronous registry writes from SDPORT callbacks.
A queued worker writes an immutable copy, so it never retains the slot extension
or re-enters the boot disk from a bus/request callback. I/O statistics refresh
at most once per 30 seconds of active traffic, rather than every 1,024 commands.
Clock, identity and error transitions still request a snapshot. There is no
new timer or log file. The focused queue test covers callback re-entry, snapshot
lifetime, busy-traffic throttling and registry-open failure.

The miniport also saves the quiesced SDHCI host/power/clock/interrupt settings
and T214 tuning registers. Resume restores configuration with SDCLK held off
until the internal clock is stable; commands, FIFO contents, DMA pointers and
old interrupt status are never replayed. A clock failure closes the DMA gate.
This covers controller context loss with card power retained, not a card power
cycle or platform-wide power loss. Fixed `ContextSaves`,
`ContextRestores` and `ResumeStatus` values join the existing diagnostic key.

The ARM64 builds and ASan/UBSan DMA/filter/write-bound/context regressions pass.
This is a performance and resume candidate: physical desktop responsiveness
and repeated sleep/wake still require measurement. Use the matching
FAT32-visible header in BL33 and the miniport; do not mix the two layouts.

### Runtime transfers and hidden-FAT comparison

Switchroot's [B01 microSD configuration](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi)
sets an 8 MiB ADMA request limit. This port uses a 512 KiB runtime buffer/request
limit, with a 64 KiB allocation fallback. Descriptors retain nonzero 32 KiB
segments and checked write bounds. Bounded diagnostics include `AdmaBufferBytes`,
`AdmaRequests`, `AdmaMaxBlocks` and `PioFallbackRequests`.

Both hidden and visible builds retain SDR104/ADMA2 and the same runtime
transfer limit. Earlier comparisons reported roughly one-minute stalls, but
on the latest trial the owner found no clear responsiveness difference.
The current card keeps FAT32 visible with package 0.5.0.3 and a matching
BL33. Further performance investigation is deferred; this is not evidence of
a completed performance fix. Keep both source profiles available.

Candidate 0.4.2.0 removes the cancelled S4 experiment. The Lite profile does
not advertise dump/hibernate support, allocate a dump DMA buffer or bypass the
I2C gate in a dump path. Normal SDR104, card identity/write bounds and the
retained-power controller-context helpers remain. Those helpers do not provide
whole-system sleep or re-identification after card power loss.

Build with `NV_T210_LITE_WINARM_WRITE=1` and `NV_T214_SDR104=1`, using the same
private card header in BL33 and the miniport. Direct retained-memory sleep/wake
is still under development. DDR200 is a separate extension; see the
[long-term goal](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/PORTING.md#windows-ddr200-goal).

### T214 DDR200 candidate (0.5.0.1)

Build with `NV_T210_LITE_WINARM_WRITE=1`, `NV_T214_SDR104=1` and
`NV_T214_DDR200=1`. The normal build keeps the extension disabled. Use the
same private visible/hidden partition header as BL33.

The candidate follows Switchroot's SD 6.x SCR and CMD6 vendor-function-14
detection. At a quiesced SDPORT tuning callback it first verifies SDR104,
selects SDR12 access mode, switches CMD6 group 2 to 14, and sets the host to
DDR50 with a 399.36 MHz module clock / 199.68 MHz card clock. It scans 128
manual taps, requires a contiguous window of at least eight, selects its
midpoint and compares four readbacks of the WINARM boot sector against the
SDR104 copy before admitting normal I/O. It does not implement HS400 DLL
synchronization or assume support from a card's brand.

Unsupported cards remain in SDR104. A failed transition, narrow window,
timeout or readback mismatch triggers one power-cycle/reinitialization to
SDR104, including tuning and volume-identity checks. The private path issues
no storage writes. It must preserve the card's CID and SDPORT-owned RCA;
otherwise it returns failure and blocks ordinary data I/O until a fresh
SDPORT enumeration, rather than pretending that recovery succeeded. Physical
fallback behavior, including any RCA change, still requires a Lite test.

Existing bounded registry diagnostics gain `Ddr200CardInfo`, `Ddr200Capable`,
`Ddr200Active`, `Ddr200Status`, `Ddr200RecoveryStatus`, `Ddr200Fallbacks`,
`Ddr200Window` and `Ddr200Tap`. Card-info bits 1/2/4 mean completed SCR/CMD6/CID
observations; the first two can be absent after reinitialization until SDPORT
reads them again. SDPORT's logical `BusSpeed=7` remains SDR104; use
`Ddr200Active` with the clock/tuning evidence to identify the private mode.

Host sanitizer tests cover capability parsing, seven/eight-tap boundaries,
clock limits, actual mode-transition code, timeouts, corrupted readbacks,
CID/RCA mismatch and both partition views. ARM64 builds pass. The signed
FAT32-visible 0.5.0.1 package was installed with Bluetooth 1.0.0.18 on
2026-11-07; its SYS/INF/CAT matched on readback. The existing visible-profile
BL33 and all physical partition boundaries were retained. These are
**code/build results, not physical DDR200 acceptance or a throughput claim**.
The owner photographed **INACCESSIBLE_BOOT_DEVICE** before the desktop on
the next boot. The returned card had crash dumps/error logging disabled and
no current driver record; the failing operation and cause remain unknown. The readback-verified
installation is therefore **not a successful boot test**. Platform sleep/wake
is isolated on [`sc7-test`](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/tree/sc7-test).

Resume candidate **0.5.0.2** also saves the active CAR source and base clock.
It restores SDMMC1's own enable/reset before accessing a lost controller,
and restores the saved SDR104 or DDR200 source/divisor before the first card
clock pulse. The previous sequence briefly enabled the saved DDR divisor on
the SDR104 source before switching back. Only SDMMC1's set/clear clock bits
are changed; old commands, DMA pointers and interrupt status are not replayed.
PLL, clock-enable, reset, source-readback or internal-clock failure blocks
data I/O. ARM64 builds for both partition views, actual-callback fault tests
and signed visible-package checks pass. These changes are included in the installed 0.5.0.3 candidate below;
physical acceptance remains pending. It covers retained card power,
not platform-wide power loss or a card power-cycle/re-identification. It does
not depend on the experimental PEP and does not establish a cold-boot fix.

Recovery candidate **0.5.0.3** fixes a reproduced event-mask loss: the private
DDR200 fallback performs a controller reset-all, which clears SDPORT's status
and signal enables. Recovery previously returned without restoring those
masks, so later interrupt-driven requests could stop completing. The callback
now masks private polling traffic, clears its pending events and restores the
caller's original masks on success and failure. The new regression failed
before the fix and passes afterwards for both partition views, including
zero masks and failed card recovery. Both ARM64 builds pass. This is a
code-level defect and a candidate fix, not proof of the photographed blue
screen's root cause or a successful hardware boot. It retains DDR200 and the
SDR104 fallback; it does not enable SC7.

The signed FAT32-visible 0.5.0.3 package was installed with Bluetooth 1.0.0.19
on 2026-11-07. SYS/INF/CAT readbacks matched. The original boot/firmware files,
other drivers, service start settings and Bluetooth pairing state were retained.
After this update, the owner confirmed **successful desktop boot and working
Bluetooth**. This accepts the observed boot and Bluetooth recovery on the
tested Lite. Actual DDR200 selection, electrical tuning/fallback and throughput
remain unverified until the runtime parameters are read; a successful boot
alone does not distinguish DDR200 from SDR104.

```sh
python3 drivers/Tegra210Sdhc/tests/test-ddr200.py
python3 drivers/Tegra210Sdhc/tests/test-ddr200-transport.py
python3 drivers/Tegra210Sdhc/tests/test-t214-context.py
python3 drivers/Tegra210Sdhc/tests/test-diagnostics.py
```

References: [Microsoft SD miniport](https://github.com/microsoft/Windows-driver-samples/tree/main/sd/miniport/sdhc)
and [Switchroot SDHCI transfer limits](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/mmc/host/sdhci.c).
