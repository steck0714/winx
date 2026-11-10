# GPU porting scope for all Nintendo Switch models

WinX's present GPU implementation is a **T214 / HDH-001 Lite bring-up**, not all-Switch support. Switchroot Android 14/15 lists Erista (`odin`), Mariko V2 (`modin`), Lite (`vali`) and OLED (`fric`) as supported device families. All are Tegra X1-family systems with GM20B-class graphics, so the GPU command vocabulary and much of Mesa NVK's Maxwell compiler/command generation are candidates for reuse. Device support in Linux or iOS does not itself establish that a Windows driver works on that board.

## Reuse boundary

| Layer | Reuse expectation | Work needed for additional boards |
| --- | --- | --- |
| Maxwell/NVK shader compiler and packet encoders | Reuse where independent of Horizon/XNU/Linux services; retain upstream license and revision | Windows build and backend integration; validate packet/firmware ABI per GPU revision |
| Generic GM20B FIFO, MMU, fence, command validation and WDDM ownership code | Share the implementation when GPU revision and address format match | Validate chip identity, aperture encoding, GPU VA limits, memory attributes and fault/reset behavior per SKU |
| T214 Lite power, clock, reset, secure firmware and ACPI resource profile | T214 Lite only until measurements prove equivalence | Separate profiles and source-backed sequences for Erista, Mariko V2 and OLED; never inherit Lite addresses/sequences by assumption |
| Panel, display transport and Windows desktop presentation | Not a GPU-core concern | Preserve each board's existing display path; validate presentation and DWM interop independently |

## Per-model entry gates

| Family | SoC/profile to confirm | Evidence required before enabling the Windows GPU device |
| --- | --- | --- |
| Erista V1 / unpatched and patched | T210 revision and bootloader memory map | Matching Linux DTB, GPU ID/revision, clocks/rails/resets, interrupts, memory/IOMMU, secure carveouts, firmware provenance, cold and warm boot behavior |
| Mariko V2 | T210B01 board profile | Same resource set from its own DTB and boot logs; compare secure-boot/firmware handoff and memory reservation against the V1 profile |
| Switch Lite | T214 / HDH-001 | Existing reference implementation; keep the accepted 729.6 MHz FIFO path and current Basic Display fallback |
| OLED | T210B01 OLED board profile | Matching OLED DTB and power/display topology; verify that its GPU resources and carveouts match neither V2 nor Lite by assumption |

## Safe expansion sequence

1. Add an immutable, compile-time board profile selected from a verified board identifier. Unknown or mismatched hardware must leave the GPU device unpublished.
2. Keep SoC-independent code in the shared core. Move power, reset, clock, resource apertures, interrupts, firmware selection and carveouts into per-profile code.
3. Bring up each profile first in read-only identity/resource audit mode. Then prove power restoration and one bounded FIFO readback; never copy a successful Lite frequency or register list to another SKU without evidence.
4. Require GPU readback, isolated Vulkan off-screen rendering, stable Win32 presentation and desktop-compositor evidence as separate milestones for every SKU.
5. Keep Microsoft Basic Display and the existing boot path available until the particular board passes its gates.

## Sources

- [Switchroot Android 14/15 supported SKUs](https://wiki.switchroot.org/wiki/android/android-14-15)
- [LineageOS Switch V1 hardware page (GM20B)](https://lineageos.github.io/lineage_wiki/devices/nx/variant1/)
- [Switchroot Linux kernel GPU implementation](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/tree/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu)
- [Linux Nouveau GM20B implementation](https://github.com/torvalds/linux/tree/v6.12/drivers/gpu/drm/nouveau/nvkm)
- [Mesa Switch / NVK](https://github.com/danfromtico/mesa-switch)
- [iOS-on-Switch](https://github.com/brunostjohn/ios-on-switch-lmao): independent XNU port useful as a hardware/software reference; its IOKit driver does not substitute for WDDM.
