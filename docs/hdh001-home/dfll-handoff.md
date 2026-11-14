# T210B01 DFLL firmware support

This change adds resources and characterization for Windows SwitchFan's CPU
AggressiveSLT DFLL implementation. It does not switch the CPU to DFLL at boot,
enable an OC profile, change the existing CPU/GPU/EMC defaults or add SC7.

`Include/Tegra214Dfll.h` defines a separate 32-byte version-1 record at
`0xA9FB0100`, within the already reserved performance handoff page. The existing
performance record at `0xA9FB0000` is unchanged. `LitePerformanceDxe.c` records
HIDREV, SKU, fused CPU Speedo, CPU process ID and the probed MAX77812 address,
then writes the record back to memory. A non-T210B01 SKU, invalid Speedo or
ambiguous PMIC probe leaves the magic invalid. The kernel validates it again.

The process threshold of 1650 is the T210B01 CPU threshold from Switchroot
kernel `2d0059fd3167a8df756de2aa0489d4aa70a9fc15`,
`drivers/soc/tegra/fuse/speedo-tegra210.c`. It is not the GPU process threshold.

The `FAN0` ACPI device gains these translated memory resources:

| Address | Bytes | Use |
|---|---:|---|
| 0x70110000 | 0x300 | DFLL controller and PMIC LUT |
| 0x6000662c | 8 | DFLL reference / SoC clock sources |
| 0x600062f4 | 4 | DVCO reset |
| 0x6000635c | 12 | bank-W status, read-only |
| 0x60006438 | 8 | bank-W reset set/clear |
| 0x60006448 | 8 | bank-W gate set/clear |
| 0x60006128 | 4 | I2C5 source, read-only |

The old CAR resource is not widened. The Windows driver needs every new
resource and the valid handoff before advertising dynamic CPU modes. Rollback
to the old firmware makes these modes unavailable; restore the Windows fixed
profile before replacing either driver or firmware.

Validation: full `LiteHome` ARM64 firmware build with EDK2 `edk2-stable202402`
(`edc6681206c1a8791981a2f911d2fb8b3d2f5768`), devkitA64 GCC 15.1.0 and iasl.
The accompanying review bundle records source/archive and binary SHA256s.
No boot/DFLL/voltage transition on real hardware is claimed. A build-local
SD dump target header is private and is not part of the source patch.


When Windows uses an existing `acpitabl.dat` DSDT override, updating firmware
alone does not expose the new FAN0 resources. `Tools/add-dfll-to-override.py`
adds only the seven DFLL memory descriptors to a disassembled FAN0 raw `_CRS`
buffer, retaining the GPU/display and all other devices. It rejects duplicate
or overlapping descriptors. Recompile with iasl and preserve the old override
for recovery. The main integration checked a byte-identical unmodified AML
roundtrip, then compiled the amended table with a valid length/checksum; it
adds84 resource bytes. This is an ACPI structure check, not a hardware pass.


Private handoff version2 (Fan16): append ClockMKhz, read from OSC_CTRL (the
Switch Lite's 38.4 MHz crystal encoding5) and SPARE_REG0[3:2]. This supplies the
clock of WinX's CLK_M-parented I2C5 without extending the kernel MMIO resource
range or reparenting a bus shared with CPU/GPU PMIC traffic. Older firmware does
not satisfy the new DFLL availability check; legacy fixed clocks remain usable.
This revision does not change the existing performance record, EMC policy or
ACPI/display table. The matched Fan16 + firmware pair requires a normal restart.
