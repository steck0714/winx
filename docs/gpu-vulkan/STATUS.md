# Native Vulkan on Switch Lite

## Goal and scope

Run Vulkan workloads on the Maxwell GM20B GPU in a Nintendo Switch Lite (HDH-001, T214) under Windows 11 ARM64. Use mesa-switch's NVK core and a native Windows kernel backend. The current Windows desktop uses CPU software rendering.

First prove GPU execution and readback, then off-screen Vulkan drawing, then stable window presentation. Evaluate Direct3D-to-Vulkan integration for desktop composition separately. Game compatibility is outside this project. Keep the bootable landscape desktop, stock GPU clocks, existing Windows installation and all SD partitions throughout bring-up.

Continue implementation, compilation and necessary host checks autonomously until the next step requires a physical Lite test. A progress report, successful build or source push is not a stopping point. Publish GPU source only to `vulkan-test`; the default documentation links here and `sc7-test` stays unchanged.

## Current stage and evidence

The first milestone is a GPU-written word read back by Windows. ACPI resources, WDDM resource ownership, GM20B entry encoders and T214 idle-GPU power sequencing are implemented. R2 reached the rail step but its firmware adapter called a read-transfer API for a standalone write. R3 uses the existing register-write API and is installed with exact readback verified; the original boot firmware remains backed up on the host. R3 hardware results and Windows GPU execution remain pending.

| Component | Code | Windows ARM64 build | On the Lite under Windows |
| --- | --- | --- | --- |
| GPU ACPI resources | Opt-in `SWITCH_LITE_GPU` / `SWOA0F01`, separate from POST display | DSDT compiled; one existing GCRS warning | Pending |
| T214 GPU power | Switchroot rail/clock/reset/MC sequence; OUT5 override and firmware PMIC write adapter corrected | ARM64 library/BL33 build pass; observed-state, unwind and adapter-contract tests pass | R2 failed at PMIC adapter; R3 identity check pending |
| GM20B command encoder | Upstream encoder unchanged; bounded Windows report wrapper | ARM64 COFF; host packet-boundary tests pass | GPU execution pending |
| WDDM resources and page entries | Acquire/release helpers; 4 KiB PTE, small-page PDE and RAMIN PDB encoding | `TegraGpuCore.lib` built with WDK/SDK 26100.6584; host address-format tests pass | Pending; not an installable miniport |
| GPU execution, VidMm integration and synchronization | Firmware loading, channels, residency and fence callbacks remain open | Pending | Pending |
| NVK Vulkan ICD and off-screen rendering | Windows backend planned | Pending | Pending |
| Win32 window presentation | Transport decision open | Pending | Pending |
| DWM Direct3D-to-Vulkan integration | Separate feasibility study after presentation | Pending | Pending |

- **Board evidence.** The card's Switchroot Ubuntu device-tree image contains a VALI rev 0 entry identifying Nintendo Switch Lite. Its enabled GM20B node lists 16 MiB apertures at `0x57000000` and `0x58000000`, 4 KiB at `0x538f0000`, stall/nonstall GIC SPIs 157/158, a GPU reset, host1x and an IOMMU reference. The Ubuntu boot log from this unit records `57000000.gpu` joining an IOMMU group and GM20B clock setup. The SHA-256 of the extracted VALI DTB is `36dd609e2bd112ffbd48c1d03b094516d4c47dca6827633db79c095ebd09913b`.
- **Windows firmware.** `WINX0001` remains the optional POST framebuffer device. The new opt-in `SWOA0F01` node exposes the three GPU apertures and GSIs 189/190; the Windows helper preserves translated interrupt vectors and unwinds partial mappings. It performs no GPU register access or power changes. Hekate security carveouts remain reserved. [Windows DSDT](../../AcpiTables/Dsdt/Dsdt.asl) · [Firmware memory map](../../Include/Device/MemoryMap.h)
- **Hardware evidence.** The owner's R2 photograph shows PMIC `33`, M1 VOUT `50`, EN_CTRL `40`, OUT5 `00030000`, power result `3`, stage `1`, restore `3`, ownership `1`; BOOT0 was not read. Source tracing confirms that `i2c_xfer_packet(..., rx_size=0)` returns before the write is sent. The restore failure has the same caller error; it is not evidence of a failed physical regulator. R3 calls `i2c_send_byte` and retains combined write/read for register reads. [Firmware adapter](../../Library/PlatformBootManagerLib/LiteGpuI2c.h)
- **Build evidence.** LLVM 23 compiled the backend as `COFF-ARM64` against WDK/SDK 26100.6584 with a 22621 target and WDDM 2.0 interfaces. Core tests cover address/packet boundaries, power ordering, observed OUT5 state and failures. The firmware adapter regression reproduced R2, then passed with the corrected API using the real I2cLib declarations. Mesa stays at `0ff52079`. R3 BL33 builds and passes entry/map checks (SHA-256 `bd097a94758f31546a01c88d7bfab583bbf61c221c43db2d605fce1a9d3e3428`). These checks do not establish GPU execution. [Backend/build command](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/tree/vulkan-test/drivers/TegraGpu)

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

GM20B power/identity still needs hardware confirmation; firmware loading, the first isolated GPU mapping and executable WDDM callbacks remain open. Presentation allocation/fence interoperability is separate. B01 uses MAX77812 M1, preserving M3/M4 and unrelated clock fields. R2 follows Switchroot's `clk_set_rate` for GPU OUT5, including bit 18 (`TEGRA_DIVIDER_FIXED` override), then restores the original output at exit. It requires a reset/clamped GPU and retains ownership on MC-drain failure. [Power sequence](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/blob/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu/os/linux/platform_gk20a_tegra.c) · [Divider implementation](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/clk/tegra/clk-divider.c)

Next physical check: select the existing `Windows 11 Lite` entry and verify the `T214 GPU POWER CHECK R3` banner (`-D SWITCH_LITE_GPU=1`, `gpu_check=1`). The probe powers the idle GPU, reads BOOT0 twice, restores owned resources and requests power-off after 30 seconds. It performs no DMA and writes no files. Record power result, stage, current OUT5, BOOT0 and restore result. Remove `gpu_check` to resume ordinary boot. This prerequisite does not satisfy Windows execution acceptance step 1.

After identity/power confirmation, connect private page-table/report memory and channel setup to the WDDM adapter, then run acceptance step 1. No GPU driver or ICD is installed by the firmware probe.

Update this page in place as evidence changes. Keep detailed traces with the relevant test and use Git for historical states.
