# Verification status and value provenance

このページをWinXの動作状況を読む基準にします。「動く」「対応する」と書く場合は、対象機種と確認方法を併記します。

## Evidence labels

| Label | Meaning |
| --- | --- |
| **Physical report** | A physical Switch result reported by the owner/contributor. It is real-device evidence, but not an independent reproduction unless a second test is stated. |
| **Physical readback** | A value or operation was read back from a named physical Switch. The model, software revision and test scope still apply. |
| **Build verified** | The named configuration compiled and produced its build artifacts. This does not prove that BL33 boots or that hardware responds. |
| **QEMU verified** | Only the listed QEMU machine and emulated devices were exercised. A generic ARM `virt` machine is not a Tegra210/214 Switch. |
| **Source-derived / theoretical** | Inferred from Hekate, Switchroot/Linux device trees or code, EDK II, or upstream driver sources. It is a reference value or implementation hypothesis, not a Switch measurement. |
| **Unverified** | No relevant runtime evidence is recorded. |

Unless a line explicitly says **Physical readback**, numerical addresses, IRQs, GPIOs, clock rates, voltages, PCIe windows and register values are **source-derived / theoretical**. A matching device-tree entry, ACPI table, source constant, successful compile or AML disassembly does not establish that the value works on a physical console.

## Current evidence

| Area | Evidence | What it establishes |
| --- | --- | --- |
| Original WinX on Erista (Icosa/T210) | **Physical report**, one console. The project owner reported Hekate-to-BL33 UEFI Shell, display, microSD access and a USB 2 keyboard. A Dell keyboard through a Type-C OTG adapter was one tested keyboard. See the [GBAtemp WinX thread](https://gbatemp.net/threads/switch-winx-a-nintendoswitchpkg-fork-using-2024-edk2-testing-development.685307/) and [original Erista notes](../README.winx.md). | Does not establish compatibility with all keyboards, other Erista units, or other Switch models. The 2026-10-28 Windows 11 ARM64-from-NTFS result in the notes is also an owner report; charging/xHCI failures and no touch response are recorded there. It is not a test of the present source build. |
| Windows on Switch Lite (Hoag/T210B01) | **Physical report/readback** on the Lite described in the [hardware status](../README.md#hardware-status) and `docs/hdh001-*`. | Windows desktop, Windows touch, Wi-Fi/Bluetooth and other rows apply to the stated Windows/driver revisions. Windows touch is not proof of UEFI touch; these results do not validate the current WinX UEFI image. |
| Optional Lite power-button wake IRQ (`switchpwrbtn` 1.0.0.5) | Source/build/model candidate; **physical IRQ-wake acceptance unverified**. | The reported physical baseline is the 1.0.0.4 polling path. Resource values such as GSI 118 remain source-derived unless separately read back on hardware. |
| Current EDK II board profiles | **Build verified** for Icosa/Erista, Hoag/Lite, Iowa/battery-enhanced and Aula/OLED using RELEASE AArch64 configurations on 2026-11-10. | No physical boot, board auto-detection, Joy-Con, touch or peripheral operation was tested by these builds. FVMAIN free space was 48, 16, 0 and 0 bytes respectively in the corresponding build reports. These are build measurements, not hardware values. |
| QEMU smoke check | **QEMU verified** with generic EDK II ARM `virt` firmware: UEFI Shell, console input/output, FAT mapping and listing `BOOTAA64.EFI`. | It did not boot the current WinX image and did not emulate Switch Tegra hardware, Joy-Con rails, touchscreen or the WinX boot menu. |
| USB keyboard input in current WinX | **Unverified on current image** beyond the historical Erista report above. The EDK II `edk2-stable202608` `UsbKbDxe` requests USB HID Boot Protocol and documents Boot Protocol-only support in [`KeyBoard.c`](https://github.com/tianocore/edk2/blob/edk2-stable202608/MdeModulePkg/Bus/Usb/UsbKbDxe/KeyBoard.c). | The historical one-keyboard result is not a broad compatibility test. Keyboards requiring Report Protocol, NKRO, unusual hubs or other layouts need separate testing. |
| UEFI touchscreen and Joy-Con controls | **Unverified** in the current WinX image. | Windows touch on Lite is a separate OS driver result. Hekate's [Joy-Con implementation](https://github.com/CTCaer/hekate/blob/master/bdk/input/joycon.c) and [Switchbrew UART mapping](https://switchbrew.org/wiki/Bus_services) are research references, not evidence that WinX has working Joy-Con input. |
| UEFI Wi-Fi, XUSB and GPU acceleration | Bring-up code or research exists, but each current WinX runtime path is **unverified** unless a test is explicitly listed elsewhere. | A Windows Wi-Fi report or a GPU FIFO readback under Windows does not prove UEFI Wi-Fi/XUSB or Vulkan rendering. See [GPU status](gpu-vulkan/STATUS.md). |
| Linux/GRUB and iPadOS/XNU from WinX | **Unverified**. | GRUB configuration/loader code is an integration path, not a successful Linux boot. The separate iOS-on-Switch project is not a WinX boot test; see [Linux status](multiboot/arm64-linux-ja.md) and [iPadOS boundary](multiboot/ipados-xnu-ja.md). |
| EfiFs formats | The upstream AArch64 EFI binaries are included in the named firmware profile; individual filesystem reads are **not all verified**. | The Erista NTFS report does not prove that every bundled EfiFs filesystem works. OS boot requires a separate loader, kernel and Switch drivers. See [EfiFs capability notes](multiboot/efifs-os-capability-ja.md). |

The later GBAtemp post in the same thread that lists touchscreen, EHCI hotplug, Wi-Fi and Windows desktop says it describes a **similar separate project**. The repository/build identity is not established there, so those claims are not counted as WinX results.

## Source basis for theoretical values

- Hekate's [fuse definitions](https://github.com/CTCaer/hekate/blob/master/bdk/soc/fuse.h) list Icosa, Iowa, Hoag and Aula hardware types. This supports the source taxonomy, not runtime identification by the current WinX firmware.
- Hekate's [hardware initialization](https://github.com/CTCaer/hekate/blob/master/bdk/soc/hw_init.c), [Joy-Con driver](https://github.com/CTCaer/hekate/blob/master/bdk/input/joycon.c) and Switchbrew's [UART mapping](https://switchbrew.org/wiki/Bus_services) are references for pinmux/UART research. Values copied from them remain theoretical until verified per board.
- XUSB and ACPI resource values copied from Switchroot/Linux device trees are source-derived. A built AML table proves only what was emitted into the table; it does not prove that Windows successfully starts the described device.
- Microsoft's [Windows 11 minimum hardware requirements](https://download.microsoft.com/download/7/8/8/788bf5ab-0751-4928-a22c-dffdc23c27f2/Minimum%20Hardware%20Requirements%20for%20Windows%2011.pdf) list ARMv8.1 atomic instructions for ARM64, while Arm's [Cortex-A57 manual](https://documentation-service.arm.com/static/5e906b9fc8052b1608760c7b) identifies the core as Armv8-A. This is a source-derived compatibility constraint, not a version-by-version boot result; the Lite's reported 22631.2861 (23H2) boot is the separately recorded physical evidence.

When new tests are added, record the board code/product, boot chain, firmware/driver revision, date, exact observation and whether it was owner-reported or independently reproduced. Keep measured readbacks separate from requested clock/voltage targets and source-derived constants.

### This working tree

The input, screenshot, PCM, WinX visual, multi-boot menu and read-only custom-entry changes in this working tree are source edits only; this revision has not been rebuilt or measured on hardware. Earlier build and QEMU results above predate these edits. Mouse latency, timer overhead, screenshot throughput, PCM sound, custom BMP presentation/BGRT handoff, multi-OS launch behavior and Doom integration remain unverified.

An earlier host GCC `-fsyntax-only` pass accepted the then-current `SwitchVisuals.c` and `SwitchBootMenu.c`, and an earlier preview SVG parsed with the Windows XML parser. That pass predates the current menu parser/redesign and does **not** verify the current source. No current AArch64 EDK II build or QEMU test was produced for these edits. Installing Clang/LLD was previously attempted, but pacman stopped before installation because the C: package volume lacked disk space. The updated menu mockup has not yet been rendered on Switch hardware.

The [driver gap audit](driver-gap-audit-ja.md) and [multi-boot menu guide](winx-multiboot-ja.md) distinguish source status from physical evidence. In particular, the T214 SD driver has SDR104 host readback but no throughput benchmark; DDR200 active-mode readback is still pending. The Windows GM20B sources do not yet provide game-capable WDDM plus a user-mode API driver.
