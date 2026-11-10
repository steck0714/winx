# ARM64 Linux firmware interface

Linux boot entries are in `Boot/grub.cfg`; keep one entry per distribution/kernel/DTB/root tuple. Linux on Switch uses board-specific device trees and kernel/initramfs support. The UEFI ACPI extension fragment for Windows is not the Linux hardware description; the selected Linux entry supplies its own Switch DTB.

The supported candidate boards and external Switchroot/Community DTB references are documented in `Platforms/Switch/README.md` and `docs/multiboot/arm64-linux-ja.md`. A successful GRUB menu does not prove the kernel supports that board.
