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

For the experimental WINARM build, set `NV_T210_LITE_WINARM_WRITE=1`. It shares the Lite PIO, clock and voltage setup, and restricts block writes to the configured 80 GiB NTFS partition and existing FAT32 data partition. This profile requires a per-card header. The generator reads a saved whole-card image and checks the MBR, primary GPT and NTFS layout. The tested layout uses hybrid MBR slot 4 and GPT slot 11. Keep the generated card identifiers local. Version 0.3.2.6 projects WINARM as the active first MBR entry and FAT32 as the second data entry, without writing the physical partition table. FAT32 writes require its matching extent and FAT32 boot-sector facts; WINARM retains its NTFS identity checks. Writes to partition tables, Linux, Android and emuMMC stay rejected.

Build the companion UEFI with the same generated header used for this
miniport. The firmware boot manager and storage driver must agree on all
512 bytes of the projected MBR. A one-entry firmware view and this two-entry
driver had different MBR checksums on the card that reproduced stop 0x7B.
UEFI reads both configured data ranges and presents WINARM as the first,
active partition. The Windows miniport separately checks the relevant boot
sector before admitting writes to each data volume.
[Windows disk signature fields](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_disk_signature).

From the repository root in PowerShell:

```powershell
$sd = 'drivers/Tegra210Sdhc'
py -3 "$sd/winarm/configure-target.py" 'D:/images/test-card.img' "$sd/winarm/generated"
Copy-Item "$sd/winarm/generated/winarm-target.h" "$sd/src/"
./tools/Build-Driver.ps1 -Wdm -Name nvtsdhc -Sources "$sd/src/nvtsdhc.c" -Libs sdport.lib -Defines 'NV_T210_LITE_WINARM_WRITE=1' -Inf "$sd/winarm/nvtsdhc.inf" -OutDir build/WINARM-R3
```

For WinPE deployment, use the matching R3 BL33, generated startnet.cmd and the signed test package. Verify the final SYS and INF against their catalog using a test certificate valid at the firmware's cold-start clock. The hardware build and test steps are in the [R3 source tree](https://github.com/ruje0504-2/winx/tree/b44da7475fb281d6e4390d976f8f8e0098154d87/docs/hdh001-winarm-r3). One HDH-001 verified the 4 KiB receipt file after shutdown and a second boot. [Hardware report](https://github.com/steck0714/winx/issues/5)
