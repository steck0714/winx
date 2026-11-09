# iPadOS/XNU boot status

The public [ios-on-switch-lmao boot guide](https://github.com/brunostjohn/ios-on-switch-lmao/blob/main/boot/README.md) describes an OLED-only Hekate → BL31/U-Boot → native loader → XNU path. The corresponding source assigns its own BL33 addresses and requires Hekate's platform setup; it is not a UEFI application that WinX can start directly. Its source and binaries are not copied here.

WinX currently has no verified XNU loader, APFS boot path, Apple Device Tree/`boot_args` handoff, or physical iPadOS boot. Do not present iPadOS as a working GRUB menu option. The generic EFI application entry in `OperatingSystems/Other/Boot/grub.cfg` is only an example hook and has not been shown to load the iPadOS project's boot chain.

**Future design requirements (theoretical):** create or adapt an EFI-compatible XNU loader, provide the required storage/filesystem access, build the Apple Device Tree and `boot_args`, and define the post-`ExitBootServices` handoff. Validate first on the OLED profile, then treat each other board as unsupported until separately tested. The [per-model integration notes](../../../docs/multiboot/ipados-xnu-ja.md) list upstream load addresses as WinX-theoretical values and credit the source projects.
