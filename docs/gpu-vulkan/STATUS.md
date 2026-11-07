# Native Vulkan on Switch Lite

## Goal and scope

Run Vulkan workloads on the Maxwell GM20B GPU in a Nintendo Switch Lite (HDH-001, T214) under Windows 11 ARM64. Use mesa-switch's NVK core and a native Windows kernel backend. The current Windows desktop uses CPU software rendering.

First prove GPU execution and readback, then off-screen Vulkan drawing, then stable window presentation. Evaluate Direct3D-to-Vulkan integration for desktop composition separately. Game compatibility is outside this project. Keep the bootable landscape desktop, stock GPU clocks, existing Windows installation and all SD partitions throughout bring-up.

## Current stage and evidence

The first milestone is a GPU-written word read back by Windows. Board resources and the reusable command encoder have been identified.

| Component | Code | Windows ARM64 build | On the Lite under Windows |
| --- | --- | --- | --- |
| GM20B command encoder | Reused the upstream report, fence and cache packet helpers unchanged | Compiled as a COFF object | GPU execution pending |
| WDDM kernel backend, memory and synchronization | Design and resource mapping | Pending | Pending |
| NVK Vulkan ICD and off-screen rendering | Windows backend planned | Pending | Pending |
| Win32 window presentation | Transport decision open | Pending | Pending |
| DWM Direct3D-to-Vulkan integration | Separate feasibility study after presentation | Pending | Pending |

- **Board evidence.** The card's Switchroot Ubuntu device-tree image contains a VALI rev 0 entry identifying Nintendo Switch Lite. Its enabled GM20B node lists 16 MiB apertures at `0x57000000` and `0x58000000`, 4 KiB at `0x538f0000`, stall/nonstall GIC SPIs 157/158, a GPU reset, host1x and an IOMMU reference. The Ubuntu boot log from this unit records `57000000.gpu` joining an IOMMU group and GM20B clock setup. The SHA-256 of the extracted VALI DTB is `36dd609e2bd112ffbd48c1d03b094516d4c47dca6827633db79c095ebd09913b`.
- **Windows firmware.** The optional `WINX0001` display node exposes the POST framebuffer. A render adapter needs its own ACPI resources. Account for the Hekate security carveouts before assigning GPU memory. [Windows DSDT](../../AcpiTables/Dsdt/Dsdt.asl) · [Firmware memory map](../../Include/Device/MemoryMap.h)
- **Build evidence.** At mesa-switch revision `0ff52079`, Clang with `--target=aarch64-w64-windows-gnu -std=c11 -O2 -ffreestanding -DNDEBUG` compiled `nouveau_horizon_gm20b.c`. `llvm-readobj` reported `IMAGE_FILE_MACHINE_ARM64` and the object exports `nouveau_horizon_gm20b_build_report`. This build covers the packet encoder only. [Exact source](https://github.com/danfromtico/mesa-switch/blob/0ff52079ca9e9e837ed27eb95a8d239f439a1864/src/nouveau/horizon/nouveau_horizon_gm20b.c)

## Implementation decisions

1. **Kernel backend.** Use Microsoft's render-only sample for WDDM device, context, paging, submission, interrupt and timeout-recovery callbacks. Implement T214 rail, clock, reset, firmware, FIFO and engine bring-up using Linux nouveau as the hardware reference. Use deko3d to check Maxwell packet ordering and fences. Start with an opt-in render adapter alongside the existing display. [Microsoft WDDM sample](https://github.com/microsoft/graphics-driver-samples/tree/de4a2161991eda254013da6c18226f5ea06e4a9c/render-only-sample/roskmd) · [nouveau Tegra platform](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/engine/device/tegra.c) · [GM20B graphics engine](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/engine/gr/gm20b.c) · [deko3d queue](https://github.com/devkitPro/deko3d/blob/373bfd43d8b2d805fa2eb7d406dbb823032df958/source/dk_queue.cpp)
2. **Memory and synchronization.** Back GPU allocations with VidMm-managed system memory. Establish the relationship between Windows allocations, GPU virtual addresses and Tegra IOMMU mappings. Track residency, CPU/GPU cache visibility, channel ownership and fence completion before recycling pages. Report completion and faults to the Windows scheduler. [WDDM GPU virtual memory](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0)
3. **ARM64 Vulkan ICD.** Keep NVK's Maxwell compiler and command generation. Implement Windows device discovery, allocation, mapping, submit and wait behind the NVK `nvkmd` interface. The Switch backend delegates those operations to Horizon services and ships a static library. Build an ARM64 Windows ICD DLL, register its JSON manifest and match its physical device to the WDDM adapter LUID. [NVK backend interface](https://github.com/danfromtico/mesa-switch/blob/0ff52079ca9e9e837ed27eb95a8d239f439a1864/src/nouveau/vulkan/nvkmd/nvkmd.h) · [Switch adapter](https://github.com/danfromtico/mesa-switch/tree/0ff52079ca9e9e837ed27eb95a8d239f439a1864/src/nouveau/vulkan/nvkmd/switch) · [Khronos loader interface](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md)
4. **Window presentation.** Implement `VK_KHR_win32_surface` and `VK_KHR_swapchain` using a presentation allocation and fence the Windows display path can consume. Mesa's current Win32 DXGI route expects a D3D12 command queue. Its other route copies through a CPU mapping and GDI. A GPU-backed presentation route must be demonstrated explicitly. [Mesa Win32 WSI](https://github.com/danfromtico/mesa-switch/blob/0ff52079ca9e9e837ed27eb95a8d239f439a1864/src/vulkan/wsi/wsi_common_win32.cpp) · [Vulkan WSI](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html)
5. **Desktop composition.** After Vulkan presentation works, evaluate a Windows Direct3D user-mode driver or translation layer backed by NVK that DXGI and DWM can actually select. Verify shared surfaces, formats and fence interoperability. Compare the same desktop workload against the software baseline using compositor frame times, CPU usage and GPU activity attributed to `dwm.exe`. [Windows composition and shared resources](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/rendering-on-a-discrete-gpu-using-cross-adapter-resources)

## Hardware acceptance in order

| Step | Evidence required from Windows on HDH-001 |
| --- | --- |
| 1. Execute and read back | Initialize a sentinel in an isolated allocation. Submit a GM20B report with a different value, observe its hardware fence, invalidate the CPU mapping as required and read the expected word. Repeat with a new value. |
| 2. Draw with Vulkan | Load the ARM64 ICD, identify the GM20B adapter and draw a known triangle off-screen. Compare GPU-produced pixels against expected foreground and background samples. |
| 3. Present reliably | Present a changing frame sequence in a landscape HWND. Record the adapter, transport, frame count and duration. Exercise resize and swapchain recreation and check for stale frames, device loss and GPU timeouts. |
| 4. Assess DWM separately | Show hardware work from the compositor and compare frame pacing with the baseline. An application drawing or presenting a triangle satisfies only the corresponding Vulkan step. |

## Blockers and next action

The Windows render node, GM20B power/firmware handoff and first isolated GPU mapping are open. The present-allocation and fence contract with the display path is open separately.

Next, map the verified VALI resources into an opt-in ACPI render node and a minimal WDDM miniport. Confirm the GPU identity and translated resources under Windows before enabling a channel. Then allocate the first report buffer and run step 1.

Update this page in place as evidence changes. Keep detailed traces with the relevant test and use Git for historical states.
