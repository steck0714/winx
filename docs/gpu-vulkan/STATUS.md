# Native Vulkan on Switch Lite

**Active, incomplete.** Physical checkpoint: **0.1.0.39 / ABI 23 / 2026-11-09 00:59:21 Pacific**. Driver `c592125`; both repositories publish only `vulkan-test`, with `WindowsDrivers` paired.

## Goal and constraints

Native Windows 11 ARM64 Vulkan on HDH-001/T214/GM20B: GPU execution/readback → off-screen Vulkan draw with known pixels → stable Win32 presentation. Desktop rendering remains software; **no native Windows NVK ICD or shader/Vulkan drawing pass exists**.

- KMD first; retain Mesa NVK's compiler, command generation and ICD. Microsoft's render-only sample supplies WDDM structure. Game compatibility is outside scope; `sc7-test` stays paused.
- Preserve landscape display, installed systems, files, partitions, BCD and BL31/BL33. Publish no proprietary firmware or signing keys.
- Keep AC/DC display-off and sleep **0 / Never**, screensaver and power-button behavior. CPU diagnostic remains `CpuOverclockEnabled=0` / 998.4 MHz / 800 mV. Black-screen investigation belongs to the side chat.
- Before a necessary restart: back up the actual Hekate configuration, temporarily auto-boot the existing Windows entry, then restore the exact configuration once Windows is online. Account for the [native restart's Hekate-menu request](../hdh001-home/power-performance.md). Recent GPU installs required no reboot.
- Continue authorized work autonomously. Only Gemini 4 read-only collaboration is permitted; earlier review calls timed out and supply no validation evidence.

## Accepted physical evidence

| Version | Result on this Lite | Limits |
| --- | --- | --- |
| .29 / ABI 17 | First 729.6 MHz FIFO semaphore/readback pass | PBDMA execution only |
| .31 / ABI 18 | GR golden save + FIFO: bind/save `0x10/1`, magic `600DC0DE`, 79,872 bytes, 19,328 changed words, topology 1 GPC / 2 TPC / 4 ZCULL | Golden initialization only |
| .34 / ABI 20 | GPU reports plus **2 ISR / 2 DPC**, GSI 190; faults/restore/owned zero | Diagnostic IRQ, not live WDDM dispatch |
| .36 / ABI 21 | Restored GR context and B197 reports `F59F3501/0A60CAFE`, **restore count 1**; faults/restore/owned zero | Graphics-engine reports, no shaders |
| **.39 / ABI 23** | **16x16 ROP clear/readback:** blue background, central 8x8 red; **all 256 pixels and 768 guards/padding words match**, `pixel_pass=true`, hash `F20AF985`. Reports `0C8E163C/F371E9C3`, 729,600 kHz, Stage 306, restore count 1, all faults/restore/owned zero | Hardware color clearing/readback; not shader execution or a Vulkan API pass |

Signed ACR/PMU/WPR, secure FECS/GPCCS, GPC-MMU, golden image and memory visibility have physical evidence. WDDM allocation/query/paging/context/queue modules compile and pass controlled native fixtures; **no render adapter is registered and no live VidMm pass exists**.
The .39 source uses separate FIFO/golden/IRQ/3D/clear IOCTLs. Its 2 MiB private GR layout includes the image, 21 rebased global patches, restricted 30-register access map and owned pixel page. CPU writes only report/pixel sentinels; acceptance requires GPU reports, L2 synchronization, complete pixel/guard comparison and retirement.
Local packet/image/failure models, sanitizers, real NETB and full ARM64 build pass. The unchanged ABI-23 tool's **59 native JSON cases** and native SYS/CAT/all 11 catalogue memberships pass; fixtures explicitly substitute device I/O. .38 trapped-method diagnostics identified the failed .37/.38 clear's extra `0x0FB8=0` write/class-error 4; .39 removes it. Details belong in the [module contracts](../../WindowsDrivers/drivers/TegraGpu/README.md) and logs.

## Installed state

- **TegraGpuProbe .39 / ABI 23 / 720 bytes / oem69.inf**, Started/OK. Matching tool: `C:\ProgramData\SwitchWOA\GpuUpdate\gr-clear-0.1.0.39\LitePerformanceCheck.exe` and Public Desktop. Read-only Desktop status at 01:00 Pacific confirms the accepted pixel result and zero owned resources.
- GPU-only DSDT: 3,676 bytes, SHA256 `b829b50944a90745fb6f6f7f44f453a6b8573fe4019a95fefd0013a983703518`; adds GPU0 LIC `0x60004414/0x1C`, retaining GSI 189/190 and all other devices. `SWITCH_LITE_GPU_IRQ=1` requires GPU opt-in. Original 3,664-byte DSDT hash: `1bac65cbe8029f40f77bf2ed00ab5808cadcba916dac4325fdc8246bf1302878`. Hash-guarded table rollback: `GpuUpdate\gpu-irq-0.1.0.32\Rollback-GpuIrqTable.ps1`; table changes need reboot.
- **Owner returned the device for GPU tests.** No GPU operation or trace remains active. The final read-only check confirms unchanged GPU table, Never settings and CPU diagnostic 0 / 998.4 MHz; no Hekate/power-policy change was needed.
- Previous GPU package/service/matched-tool backups (.34/.36/.37/.38 and older) remain under local/remote `GpuUpdate` rollback directories. Preserve side chat's uncommitted `WindowsDrivers/drivers/SwitchPowerButton` files; last observed PowerButton is .6/runtime 6, legacy mode/ready 0. Do not stage side files with GPU work.

## Next work

1. Implement shader-driven drawing using the accepted GR/image/render-target path, then connect it to NVK commands. The color-clear pass does not fulfill the Vulkan draw gate.
2. Complete the render-only WDDM adapter: real dispatch, VidMm residency/command validation, ISR/DPC completion, preemption and TDR, retaining the POST display. Existing callback modules need adapter integration and live validation.
3. Add Windows `nvkmd` discovery/allocation/mapping/submission/waits, build ARM64 ICD DLL/JSON and match the WDDM LUID. Require an actual Vulkan off-screen draw with known foreground/background pixel readback.
4. Add stable landscape HWND presentation with resize/recreation checks, then separately evaluate DWM interop/shared surfaces/fences and compositor GPU work. A CPU-copy GDI route is not GPU-backed presentation.

## Resume and evidence lookup

Keep this entry short; replace stale state instead of appending chronology. Read a specific module/log only as needed. Detailed earlier history: `git show 4a82145:docs/gpu-vulkan/STATUS.md`. Current local evidence root: `/private/tmp/t214-gpu-round`.

- Physical: `gr31-physical-result.json`, `irq34-physical-result.json`, `gr36-physical-result.json`, **`clear39-physical-result.json` + `clear39-physical-run-meta.json`**. Failed-clear diagnosis: `clear38-physical-result.json`.
- Latest build/sign/install: `clear39-{build,stage,signature-verify,install-result}.log`; matched tool fixture `clear38-native-json.log`; final device snapshot `clear39-handoff.log` and `clear39-desktop-status.json`. Package `clear39-package`, remote `GpuUpdate\gr-clear-0.1.0.39`; table proof/comparison `irq32-acpi/`.
- SSH: `ssh -i ~/.ssh/dsh_handheld_ed25519 ruje@192.168.8.6`. Direct `C:\Users\Public\Desktop\LitePerformanceCheck.exe --gpu-json` only queries. Separate run flags: `--gpu-729`, `--gpu-gr-729`, `--gpu-irq-729`, `--gpu-3d-729`, `--gpu-clear-729`; capture stdout synchronously. Query exit 0 alone is not a GPU pass. SSH timeout alone is not a confirmed hang.

References: [pinned Mesa/NVK](https://github.com/danfromtico/mesa-switch/tree/0ff52079ca9e9e837ed27eb95a8d239f439a1864/src/nouveau/vulkan/nvkmd) · [Switchroot nvgpu](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/tree/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu) · [Linux nouveau v6.12](https://github.com/torvalds/linux/tree/v6.12/drivers/gpu/drm/nouveau/nvkm) · [WDDM sample](https://github.com/microsoft/graphics-driver-samples/tree/de4a2161991eda254013da6c18226f5ea06e4a9c/render-only-sample/roskmd) · [deko3d](https://github.com/devkitPro/deko3d/blob/373bfd43d8b2d805fa2eb7d406dbb823032df958/source/dk_queue.cpp).
