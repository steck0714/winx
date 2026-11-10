# Windows firmware interface

Common Windows device descriptions live in `Acpi/SwitchExtensions.asl`, included inside the common DSDT scope. They describe opt-in SWOA devices and the Wi-Fi endpoint. Hoag (Switch Lite) GPU/display/SIO resources live under `Platforms/Switch/t214/Hoag/Acpi/` and are included only by matching Hoag feature flags. Hardware initialization remains in board/platform drivers; ACPI visibility must not make an unpowered controller appear usable.

Windows kernel driver packages remain under `WindowsDrivers/`. Keep ACPI `_HID`/`_CID`, resource windows, interrupts and driver INF hardware IDs in sync. The generic DSDT still supplies CPU/GIC/timer/UART/SD controller descriptions; Windows-specific devices are gated by build flags.
