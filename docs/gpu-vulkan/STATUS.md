# Native Vulkan on Switch Lite

**Active; long-term goal incomplete.** Latest physical checkpoint: 2026-11-08 (Pacific), firmware `4a82145`, paired driver `163b7f6`, both `vulkan-test`.

## Goal and constraints

Implement native Windows 11 ARM64 Vulkan on HDH-001/T214/GM20B: GPU execution/readback → off-screen Vulkan drawing with known pixels → stable Win32 presentation. Evaluate Direct3D-to-Vulkan/DWM separately. The desktop currently renders in software; no runnable native Vulkan ICD or shader/drawing pass exists.

- KMD first; keep Mesa NVK as compiler/command generator/ICD. Microsoft's render-only sample supplies WDDM structure, not a GPU implementation. Game compatibility is outside scope.
- Push **only `vulkan-test` in both repositories**, pairing `WindowsDrivers`. Keep `sc7-test` paused. Preserve display, installation, other systems, partitions, BCD and BL31/BL33; never publish signing keys or proprietary firmware.
- Preserve current **Never (AC/DC 0)** display/sleep settings, screensaver and power-button behavior. Black-screen testing belongs to the side chat. At the last GPU snapshot, CPU diagnostic remained `CpuOverclockEnabled=0` / 998.4 MHz / 800 mV.
- Before a necessary restart: back up the actual Hekate configuration, temporarily select automatic boot into the existing Windows entry, then restore the exact configuration once Windows is online. Account for the [native restart's Hekate-menu request](../hdh001-home/power-performance.md); no Hekate setting was changed in the latest GPU run.
- Continue authorized local implementation autonomously. Gemini 4 read-only review is permitted; no other collaboration agents. Its review attempts timed out without conclusions, so they supply no validation evidence.

## Accepted evidence

| Checkpoint | Physical Lite result | Limits |
| --- | --- | --- |
| 0.1.0.29 / ABI 17 | First 729.6 MHz FIFO execution/readback pass | No shaders |
| 0.1.0.31 / ABI 18 | GR golden save + subsequent FIFO pass at 21:46:19 Pacific; bind/save `0x10/1`, image `600DC0DE`, 79,872 bytes, 19,328 changed body words, hash `C28E60AD`; topology 1 GPC / 2 TPC / 4 ZCULL | No graphics-engine workload or Vulkan |
| **0.1.0.34 / ABI 20** | IRQ run at **23:38:04 Pacific**: exit 0, Stage 306, `fifo_pass=true`, `irq_pass=true`, measured 729,600 kHz; GPU report/readbacks `1FAA904A/E0556FB5`, **2 ISR / 2 DPC**, GSI 190; fault/result/restore/owned zero | Diagnostic notifications; live WDDM dispatch/IRQ integration and TDR still pending |

Signed ACR authentication, PMU/WPR acknowledgement, secure FECS/GPCCS startup, GPC-MMU, memory visibility and GR golden save have physical evidence. WDDM allocation/query/paging/context/queue modules compile and pass controlled native fixtures; **no render adapter is registered and no live VidMm pass exists**.
The accepted .34 IRQ fix handles full MC/LIC bitmaps and preserves first-failure diagnostics. Its host models, sanitizers, real NETB, ARM64 build, 34-case native JSON fixture and native SYS/CAT/11-member signing checks pass; fixtures explicitly substitute OS/device/GPU behavior.
**Source WIP, uninstalled:** .35 / ABI 20, driver `f3e41de`, prepares per-channel GR image headers, 21 rebased global patches and the pinned 30-register/512 KiB access map in an extended 2 MiB layout. Golden immutability, 40-bit VA encoding, read-only map/patch PTEs and failure/visibility models, sanitizers, real NETB and full ARM64 build pass locally (`gr35-build.log`). The new loader is not wired to a probe IOCTL or WDDM dispatch; no physical context restore or 3D workload is established. See the [module contracts](../../WindowsDrivers/drivers/TegraGpu/README.md).

## Installed snapshot and hardware ownership

- Last GPU window: **TegraGpuProbe 0.1.0.34 / ABI 20 / 616 bytes / oem65.inf**, Started/OK. Matching tool is in `C:\ProgramData\SwitchWOA\GpuUpdate\gpu-irq-0.1.0.34` and Public Desktop. Package/service/matched-tool backups for .31/.32/.33, plus older rollback packages, remain locally and under `GpuUpdate`.
- The passing run loaded a GPU-only 3,676-byte DSDT override: SHA256 `b829b50944a90745fb6f6f7f44f453a6b8573fe4019a95fefd0013a983703518`, adding only GPU0 LIC `0x60004414/0x1C`; GSI 189/190 and other devices remain unchanged. `SWITCH_LITE_GPU_IRQ=1` requires `SWITCH_LITE_GPU=1`. The old resource profile rejects IRQ runs before GPU power-up (Win32 50) and still supports FIFO/GR.
- **Hardware is released to the side chat for a test group. No GPU test/trace/reboot remains active. Do local work until the owner returns the device.** The side chat corrected its earlier snapshot: it now reports the passing GPU-only override is retained, PowerButton 6 mode/ready 0, and CPU IdleDisable AC/DC 0 before its temporary comparison. This task has not re-queried the device. Recheck resources, override and settings on return; do not overwrite another test's table.
- Original DSDT: 3,664 bytes, SHA256 `1bac65cbe8029f40f77bf2ed00ab5808cadcba916dac4325fdc8246bf1302878`. Hash-guarded GPU-table rollback: `GpuUpdate\gpu-irq-0.1.0.32\Rollback-GpuIrqTable.ps1`. A table change needs a reboot to become active.
- Preserve the side chat's uncommitted `WindowsDrivers/drivers/SwitchPowerButton` changes; do not stage them with GPU work. Last observed PowerButton was .6/runtime 6 in legacy mode/ready 0. Detailed non-GPU state and black-screen chronology are in the historical status, not this GPU task.

## Next work

1. Connect the prepared per-context image to a distinct graphics submission path and validate an engine-produced report. A PBDMA fence alone cannot prove GR execution. Keep the accepted FIFO, golden and IRQ entry points distinct; no CPU path may write an expected completion value.
2. Connect memory, context and queue modules to a registered render-only WDDM adapter: real dispatch/residency/command validation, ISR/DPC completion, preemption and TDR. Keep the working POST display.
3. Implement Windows `nvkmd` discovery/allocation/mapping/submission/waits, retaining NVK compilation and command generation. Build ARM64 ICD DLL/JSON, match the WDDM LUID, then require an off-screen draw with known foreground/background pixel readback.
4. Add stable landscape HWND presentation with resize/recreation checks; only then evaluate DWM interop/shared surfaces/fences and measured compositor GPU work. Mesa's CPU-copy GDI route is not GPU-backed presentation.

## Resume and evidence lookup

Read this file first. Open a module's [contracts/build instructions](../../WindowsDrivers/drivers/TegraGpu/README.md) or a specific log only when needed; keep this status concise and replace stale state instead of appending chronology.
Detailed pre-cleanup history is preserved in `git show 4a82145:docs/gpu-vulkan/STATUS.md`; no history was deleted from Git. Current local evidence root: `/private/tmp/t214-gpu-round`.

- GR pass: `gr31-physical-result.json`. IRQ pass: `irq34-physical-result.json` + `irq34-physical-run-meta.json`; failure diagnosis: `irq33-physical-result.json`.
- Latest build/sign/install: `irq34-build.log`, `irq34-stage.log`, `irq34-signature-verify.log`, `irq34-install-result.log`; tool/ABI fixture: `irq33-native-json.log`. Native tests are distinguished from physical passes above.
- GPU table/source comparison/load proof: `irq32-acpi/`; packages: `irq34-package`, remote `GpuUpdate\gpu-irq-0.1.0.34`. Black-screen traces/rollback evidence remain in their existing local/remote folders and prior Git history.
- Once hardware is free: `ssh -i ~/.ssh/dsh_handheld_ed25519 ruje@192.168.8.6`. Direct CLI `C:\Users\Public\Desktop\LitePerformanceCheck.exe --gpu-json` is read-only; `--gpu-729`, `--gpu-gr-729`, `--gpu-irq-729` each run one separate diagnostic. Capture inherited stdout synchronously; status exit 0 alone is not a GPU pass. SSH timeout alone is not a confirmed hang.

References: [pinned mesa-switch/NVK](https://github.com/danfromtico/mesa-switch/tree/0ff52079ca9e9e837ed27eb95a8d239f439a1864/src/nouveau/vulkan/nvkmd) · [Switchroot nvgpu](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/tree/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu) · [Linux nouveau v6.12](https://github.com/torvalds/linux/tree/v6.12/drivers/gpu/drm/nouveau/nvkm) · [render-only sample](https://github.com/microsoft/graphics-driver-samples/tree/de4a2161991eda254013da6c18226f5ea06e4a9c/render-only-sample/roskmd) · [deko3d](https://github.com/devkitPro/deko3d/blob/373bfd43d8b2d805fa2eb7d406dbb823032df958/source/dk_queue.cpp).
