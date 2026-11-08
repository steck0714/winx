# Native Vulkan on Switch Lite

## Goal and boundaries

Implement native Windows 11 ARM64 Vulkan acceleration for Switch Lite HDH-001/T214 using Mesa NVK and a Windows GPU kernel backend. The desktop still uses **CPU software rendering**.
Prove GPU execution/readback, then off-screen Vulkan drawing, then stable window presentation. Evaluate DWM's Direct3D-to-Vulkan path separately; a Vulkan triangle is not proof of desktop acceleration.
Do not expand game compatibility. Preserve the working landscape display, Windows installation, other systems, partitions and files.
Use **729.6 MHz** by default; **1228.8 MHz** is optional in the existing `LitePerformanceCheck`. Neither profile has an execution/stability pass.
Publish GPU source only to both repositories' `vulkan-test` branches and pin the firmware's driver submodule to the paired revision. Leave default and `sc7-test` branches unchanged.
Continue local implementation and necessary validation autonomously. Consolidate corrections before a hardware handoff; do not request a card swap for each small probe change. No reliable completion date is available.
Keep this file as the single rolling progress entry, about 100 lines or fewer. History belongs in Git; detailed logs are read only when needed.

## Current stage

**Milestone 1 remains unproven: no GPU-written fence has completed under Windows.**
Installed probe **0.1.0.8 / ABI 4** failed four consecutive 729.6 MHz tests at **Stage 304 / E6**. Both words stayed `DEADBEEF`; USERD GET/PUT stayed `0/1`, CCSR was `11000001`, and the runlist had one entry.
PBDMA statuses `10000004`/`10000000` identify no valid channel zero. Their varying GP base/fetch/GET/PUT readings are idle/context-switch SRAM, **not execution progress or a proven RAMFC mismatch**.
Scheduler and reported FIFO/MMU/PBDMA errors were zero; power/clock restoration returned zero with no retained ownership. BOOT0 was `12E000B1`; the default clock measured 729.6 MHz at 800 mV. Earlier optional-profile checks measured about 1227.8 MHz at 925 mV.
Local candidate **0.1.0.10 / ABI 5** combines saved-context bootstrap, idle-slowdown suppression, FIFO clock/timeouts, LTC/FB routing and two FB flushes. It suppresses invalid PBDMA counters and records BUS/PRI failures. **It is not installed**; no repeated 0.1.0.8 test is requested.
The WDDM library now includes allocation lifecycle callbacks and an ordered work queue, retiring hardware/context ownership before scheduler notification. This is source-only backend progress, not a registered render adapter or a hardware pass.

| Component | Code state | Build/local evidence | Physical Lite evidence |
| --- | --- | --- | --- |
| ACPI resources | Opt-in `SWOA0F01`, separate from POST display | DSDT builds with the existing GCRS warning | Device start/register access pass; IRQ untested |
| T214 power and clock | Switchroot rail/reset/MC sequence; C1 NA PLL and unwind | ARM64 build and ordering/failure models pass | Power/identity/restoration and clock measurement pass; sustained load untested |
| FIFO diagnostic | Private arena, one channel, two semaphore releases; consolidated cold initialization | 0.1.0.10 builds, signs and loads in native ARM64 WinPE | Installed 0.1.0.8 still fails Stage 304/E6 |
| Allocations | Real-WDK create/destroy/open/close DDIs; opaque storage ABI; VidMm system-memory backing | ARM64 build; WinPE batch rollback/shared-reference/destruction tests pass | Live VidMm allocation and UMD mapping pending |
| Page tables and paging | Bounded table segment/root; initial alias validation; ordered PTE/TLB records | ARM64 build; native WinPE controlled tests pass | Live VidMm residency and GPU access pending |
| Contexts and command memory | WDK process/device/context/root DDIs; private RAMIN; reserved privileged GPU VA | ARM64 build; mapping/lifetime/packet tests pass | Hardware privilege enforcement pending |
| Queue, channel and completion | Ordered admission; bind/submit; fence poll; quiesce-before-notify; reset/cancel | ARM64 build; native WinPE queue/reuse/failure tests pass | Adapter registration, real dispatch and IRQ/TDR integration pending |
| GR/shaders and NVK ICD | Native Windows integration remains open | Pending | Pending |
| Win32 presentation | GPU-backed transport decision remains open | Pending | Pending |
| DWM acceleration | Separate Direct3D feasibility/interop work after presentation | Pending | Pending |

## Decisions and evidence

- **Hardware reference.** This unit's Switchroot Ubuntu DT identifies VALI/Switch Lite and enabled GM20B. It exposes GPU apertures at `0x57000000`, `0x58000000`, `0x538f0000`, stall/nonstall SPIs 157/158, reset and IOMMU resources. Windows exposes narrow translated resources while preserving POST display and Hekate carveouts. [DSDT](../../AcpiTables/Dsdt/Dsdt.asl) · [Memory map](../../Include/Device/MemoryMap.h)
- **Physical memory.** Initial buffers are driver-owned, contiguous, uncached and below 4 GiB. Switchroot tags IOMMU addresses with bit 34 and leaves physical addresses untagged. Global GPU/GPUB ASIDs are not rewritten. This is not a Windows IOMMU domain or authorization for arbitrary DMA. [Switchroot addressing](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/blob/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu/common/mm/nvgpu_mem.c)
- **Cold initialization.** The local candidate follows bus/private-ring, FIFO reset/timeouts, LTC count routing, FB setup and BAR1/FB-flush order. Force-run profiles and idle-slowdown suppression remain until a PMU governor exists. Security-fuse policy is only read; failures prevent submission. These close source gaps, not established physical root causes. [Frontend](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/vulkan-test/drivers/TegraGpu/platform/t214_frontend.c)
- **Runlist/bootstrap.** The candidate retains nouveau's Maxwell instance-page word; Switchroot's bare-channel entry differs. This discrepancy remains unresolved by physical acceptance. First GP_PUT is preloaded in RAMFC/USERD; only the second diagnostic fence uses the BAR1 doorbell. [Maxwell runlist](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/nouveau/nvkm/engine/fifo/gm107.c)
- **Allocation lifecycle.** ABI 1 describes byte size and optional CPU visibility, never physical addresses. Real-WDK create/destroy/open/close callbacks select the implicit system-memory segment following the Microsoft GPUVA sample, with only metadata owned by the KMD. Bulk failures roll back new objects without destroying existing resources. Open handles retain Dxgkrnl references until close; local references are released before the OS callback that can synchronously destroy an allocation. Native WinPE tests cover rollback, shared opens, read-only/range checks and immediate destruction. Pages, residency and GPU VA remain VidMm-owned; live adapter/UMD integration is still pending. [Allocation backend](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/vulkan-test/drivers/TegraGpu/windows/allocation.c)
- **VidMm integration.** GM20B tables need 128 KiB backing, so use a dedicated nonzero CPU-visible segment marked `PopulatedFromSystemMemory`; no duplicate suballocator. Validate complete spans and aliases. BuildPagingBuffer snapshots private records; only ordered execution publishes PTEs before TLB MMIO. Partial progress is not completed paging. Residency, actual adapter dispatch and paging fences still need integration. [Paging backend](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/vulkan-test/drivers/TegraGpu/windows/paging_buffer.c) · [WDDM table contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_page_table_level_desc)
- **Internal GPU VA.** CreateProcess reserves one 64 MiB leaf coverage with UMD mapping disabled. A private leaf maps only privileged ring, completion and USERD/fence pages; RAMIN/runlist remain unmapped. Restore reserved PDEs invalidated by VidMm. Emit a USER workload followed by a KERNEL completion packet with WFI enabled. Physical enforcement is unverified. [Internal VA](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/vulkan-test/drivers/TegraGpu/windows/internal_va.c) · [Packet encoder](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/vulkan-test/drivers/TegraGpu/core/submission.c)
- **Context ownership.** Capture retains context/root state independently of activation; admitting later work cannot overwrite the active marker. Activation restores RAMIN and initializes only the complement of the completion value. Failed root changes prevent future stale-root use. No CPU implementation writes a successful GPU completion value. [Contexts](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/vulkan-test/drivers/TegraGpu/windows/context.c)
- **Ordered queue.** Dxgkrnl supplies nonpaged private storage for each DMA submission; admission allocates nothing and accepts no UMD-private payload yet. One worker copies the selected record into driver-owned storage, submits and polls it, then retires the channel before notification. Windows may immediately reuse its original private buffer. Reset failures retain affected references; canceled pending fences never advance completion. [Queue](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/vulkan-test/drivers/TegraGpu/windows/work_queue.c) · [Private-buffer contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_submitcommandvirtual)
- **Notification and recovery.** Matching the marker is insufficient by itself: disable/preempt, runlist removal, unbind, FB flush and TLB invalidation precede reference release and VidSch notification. Synchronization failure retains an engine-only notice for retry/reset. DMA recovery requires MC drain and clamp/reset readback. No caller-supplied retirement boolean bypasses hardware retirement. [Worker](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/vulkan-test/drivers/TegraGpu/windows/submit_worker.c) · [Channel](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/vulkan-test/drivers/TegraGpu/core/channel.c)
- **Local tests.** Host checks cover formats, clocks/power, cold initialization and failure paths. Real-WDK ARM64 WinPE tests cover allocation batch rollback/shared lifetime, paging multipass/aliases, distinct context roots, ordered submissions, private-buffer reuse, preempt/reset timeout retention, resume admission and notification cancellation. OS/register operations and simulated GPU writes are explicit substitutes; these tests do not exercise live Dxgkrnl allocation or a physical GPU.
- **Scheduler boundary.** The queue is a compiled backend, not an exposed `SubmitCommandVirtual` DDI. Worker statuses cannot be returned directly through that callback. The Microsoft render-only sample leaves virtual submission and TDR callbacks unimplemented; dispatcher, malformed-buffer fence handling, command validation, residency and IRQ/preemption/TDR wiring remain required. [Submission contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual)

## Remaining implementation

1. Connect the queue, paging and ownership modules to a complete render-only WDDM adapter alongside the existing display. Implement real dispatch, residency/command validation, interrupt completion and scheduler preemption/TDR handling.
2. Complete GR firmware/engine initialization and memory/synchronization behavior using nouveau, Switchroot and deko3d. Keep the first physical milestone limited to actual GPU execution/readback before claiming shaders work.
3. Port NVK's `nvkmd` discovery, allocation, mapping, submission and wait operations to Windows. Keep NVK's compiler/command generation. Build the ARM64 ICD DLL/JSON and match its device to the WDDM LUID; the Switch backend's Horizon services are not a Windows implementation.
4. Implement `VK_KHR_win32_surface`/swapchain presentation. Mesa's DXGI route requires a D3D12 command queue; its GDI route copies through CPU memory. Demonstrate the chosen GPU-backed route explicitly.
5. Separately evaluate Direct3D-to-Vulkan adaptation that DXGI/DWM can select, including shared surfaces and fence interoperability. Compare compositor frame time, CPU usage and GPU work attributed to `dwm.exe`.

## Physical acceptance gates

| Gate | Required evidence from this Lite under Windows |
| --- | --- |
| GPU execution/readback | Isolated sentinel allocation, submitted GPU report, hardware completion and correct readback; repeat with a new value. |
| Vulkan drawing | ARM64 ICD identifies GM20B and draws off-screen; expected foreground/background pixels match. |
| Stable presentation | Changing landscape HWND frames; bounded-duration run, resize and swapchain recreation without stale frames/device loss/timeouts. |
| DWM evaluation | Demonstrable compositor GPU work and measured frame pacing against the software baseline; application rendering alone does not satisfy this gate. |

## Installed state and next action

The card remains on **TegraGpuProbe 0.1.0.8**, the ABI 4 performance tool, working **SwitchFan 1.0.0.10**, and the existing BL33. No SD, BCD or partition changes were made for the source-only queue/allocation work.
BL33 SHA-256: `91b2acbccd925a3a4253226680692d524cba2ac60a8b89e62b3e3cd956f8364b`.
Candidate **0.1.0.10 / ABI 5 (196 bytes)** separately passed ARM64 driver/UI compilation, native WinPE DriverEntry loading and SYS/catalog signature checks. It remains local; later WDDM-library changes do not imply a rebuilt or installed diagnostic package.
Driver images use the working resident kernel-section profile and a CA:FALSE code-signing leaf under the existing development root. PnPUtil import alone does not prove valid signature/device binding.
The tool overwrites fixed `LastResult`, `LastResultTime` and successful `SelectedKhz` registry values; it creates no per-sample files. Staging copies listed payload bytes only, excluding `.DS_Store`, `._*` and `__MACOSX`.
Next local work is complete adapter dispatch and command/residency validation, then IRQ/preemption/TDR integration and the NVK Windows backend. Keep the consolidated diagnostic available for the next justified physical handoff; do not repeat the installed failure or ship each isolated correction.

## Pinned references

- [mesa-switch / NVK](https://github.com/danfromtico/mesa-switch/tree/0ff52079ca9e9e837ed27eb95a8d239f439a1864/src/nouveau/vulkan/nvkmd) — compiler and backend interface.
- [Microsoft render-only sample](https://github.com/microsoft/graphics-driver-samples/tree/de4a2161991eda254013da6c18226f5ea06e4a9c/render-only-sample/roskmd) — WDDM framework, not Raspberry Pi hardware or software rendering.
- [Switchroot nvgpu](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/tree/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu) and [Linux nouveau v6.12](https://github.com/torvalds/linux/tree/v6.12/drivers/gpu/drm/nouveau/nvkm) — GPU hardware sequences.
- [deko3d queue](https://github.com/devkitPro/deko3d/blob/373bfd43d8b2d805fa2eb7d406dbb823032df958/source/dk_queue.cpp) — Maxwell command ordering and synchronization.
- [Backend source/build instructions](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/tree/vulkan-test/drivers/TegraGpu) — detailed module contracts and tests.
