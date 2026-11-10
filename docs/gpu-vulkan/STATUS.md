# Native Vulkan on Switch Lite

> Evidence scope: physical results below are from the paired `vulkan-test` project branch. They are not WinX firmware or iPadOS tests. Values inferred from source, build configuration, or documentation remain theoretical unless labeled as a physical result. See the [verification guide](../verification-status.md).

The paired project reports a native Vulkan triangle on Switch Lite (HDH-001 / T214 / GM20B) with an off-screen readback. DWM acceleration and hardware presentation remain open. This report must not be generalized to every Switch model or to WinX.

## Reported physical results

| Build | Reported result | What it does not establish |
| --- | --- | --- |
| .87 | A one-boot KMD accepted NVK packets of 7,500, 148, and 1,556 bytes. Draw fence 27 completed; recovery status was 0. Readback matched 300 red pixels, 724 blue pixels, and 128 guard bytes. | WinX integration, Windows DWM, display presentation, or other Switch models. |
| .59 / ABI 31 | Ordered page-table updates, TLB flush, fill/copy and draw diagnostics completed; both 4,000-byte regions and all 48 guard words matched. | A Vulkan API draw, live VidMm, or a general-purpose graphics driver. |

These values are reported by that project; they have not been reproduced by this WinX worktree. Build and host fixture results are not physical-device evidence.

## Current limits and next work

- The paired project still lists general NVK command-class, address, and residency validation, plus per-engine reset and resubmission, as open.
- DWM and hardware presentation need the Direct3D resource/draw path, shared surfaces, monitored fences, and display integration.
- GM20B code can be shared across T210/T214 only where the hardware contract matches. Board clocks, power, interrupts, memory and display paths require separate source evidence and device validation. See the [Switch GPU SKU matrix](SKU-PORTING.md).
- WinX's current iPadOS work is design/status documentation. It does not contain a verified XNU loader or prove iPadOS boots from WinX.

## References

- [Pinned Mesa/NVK](https://github.com/danfromtico/mesa-switch/tree/0ff52079ca9e9e837ed27eb95a8d239f439a1864/src/nouveau/vulkan/nvkmd)
- [Switchroot nvgpu](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/tree/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu)
- [Linux Nouveau v6.12](https://github.com/torvalds/linux/tree/v6.12/drivers/gpu/drm/nouveau/nvkm)
- [Microsoft render-only WDDM sample](https://github.com/microsoft/graphics-driver-samples/tree/de4a2161991eda254013da6c18226f5ea06e4a9c/render-only-sample/roskmd)
- [deko3d queue synchronization](https://github.com/devkitPro/deko3d/blob/373bfd43d8b2d805fa2eb7d406dbb823032df958/source/dk_queue.cpp)