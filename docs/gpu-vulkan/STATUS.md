# Native Vulkan on Switch Lite

**Active, incomplete.** Physical checkpoint: **0.1.0.46 / ABI 24 / 2026-11-09 03:58:47 Pacific**. Installed driver `3a8d5bc`; both repositories publish only `vulkan-test`, with `WindowsDrivers` paired.

## Goal and constraints

Native Windows 11 ARM64 Vulkan on HDH-001/T214/GM20B: GPU execution/readback → off-screen Vulkan draw with known pixels → stable Win32 presentation. Desktop rendering remains software; **fixed NAK shaders now draw physically, but no native Windows NVK ICD or Vulkan API draw pass exists**.

- KMD first; retain Mesa NVK's compiler, command generation and ICD. Microsoft's render-only sample supplies WDDM structure. Game compatibility is outside the current triangle phase; `sc7-test` stays paused.
- Immediate priority: **real Vulkan API triangle + known pixel readback**, then a visible windowed triangle. Tell the owner to prepare video recording **before starting the visible demonstration**, and wait until they are ready. Future OpenGL compatibility should use **Mesa Zink → NVK → KMD**; no separate native OpenGL driver is planned.
- Preserve landscape display, installed systems, files, partitions, BCD and BL31/BL33. Publish no proprietary firmware or signing keys.
- Keep AC/DC display-off and sleep **0 / Never**, screensaver and power-button behavior. CPU diagnostic remains `CpuOverclockEnabled=0` / 998.4 MHz / 800 mV. Black-screen investigation belongs to the side chat.
- Owner-selected UMA budget: **512 MiB maximum GPU working set**, allocated dynamically within Windows/VidMm budgets; the earlier 1 GiB pressure-test target is withdrawn. No boot-time RAM carve-out. Current Windows-visible RAM is 3,787.5 MiB; this budget is not implemented by the private diagnostic.
- Before a necessary restart: back up the actual Hekate configuration, temporarily auto-boot the existing Windows entry, then restore the exact configuration once Windows is online. Account for the [native restart's Hekate-menu request](../hdh001-home/power-performance.md). Recent GPU installs required no reboot.
- Continue authorized work autonomously. Only Gemini 4 read-only collaboration is permitted; earlier review calls timed out and supply no validation evidence.

## Accepted physical evidence

| Version | Result on this Lite | Limits |
| --- | --- | --- |
| .29 / ABI 17 | First 729.6 MHz FIFO semaphore/readback pass | PBDMA execution only |
| .31 / ABI 18 | GR golden save + FIFO: bind/save `0x10/1`, magic `600DC0DE`, 79,872 bytes, 19,328 changed words, topology 1 GPC / 2 TPC / 4 ZCULL | Golden initialization only |
| .34 / ABI 20 | GPU reports plus **2 ISR / 2 DPC**, GSI 190; faults/restore/owned zero | Diagnostic IRQ, not live WDDM dispatch |
| .36 / ABI 21 | Restored GR context and B197 reports `F59F3501/0A60CAFE`, **restore count 1**; faults/restore/owned zero | Graphics-engine reports, no shaders |
| .39 / ABI 23 | 16x16 ROP clear/readback: all 256 pixels and 768 guards match, hash `F20AF985`; reports `0C8E163C/F371E9C3`, faults/restore/owned zero | Hardware color clear; no shaders |
| **.41 / ABI 24** | **NAK VS/FS shader draw/readback:** blue clear then shader-drawn central 8x8 red, `shader_pass=true`; **256 pixels and 768 guards match**, image/code hashes `F20AF985/FB041DD2`. Reports `556A590A/AA95A6F5`, 729,600 kHz, Stage 306, restore count 1, faults/restore/owned zero | Fixed private shader diagnostic, not a Vulkan API or live WDDM pass |
| **.46 / ABI 24** | Shared BAR1 USERD remap to a different owned physical page, then hot draw/retirement: **256 pixels and 768 guards match**, hashes `F20AF985/FB041DD2`, reports `AC742EBE/538BD141`, `bar1_phase=7`, GP_GET/PUT 2/2, 729,600 kHz, Stage 306. GR context `000FDE3E` has VALID clear; faults/restore/owned zero | Private shader arena; complete WDDM startup/queue/packed contexts and Vulkan API remain unverified |

Signed ACR/PMU/WPR, secure FECS/GPCCS, GPC-MMU, golden image and memory visibility have physical evidence. WDDM allocation/query/paging/context/queue modules compile and pass controlled native fixtures; **no render adapter is registered and no live VidMm pass exists**.
Separate FIFO/golden/IRQ/3D/clear/draw IOCTLs retain the 2 MiB GR layout, 21 global patches, restricted 30-register access map, pixel page and two GPU-read-only code pages. `tools/build_draw_shaders.py` reproducibly builds unchanged pinned Mesa NAK in an isolated host overlay: descriptor-free SM53 VS/FS 160/64 bytes, 24 GPRs each, no scratch/constants. CPU writes code/headers and report/pixel sentinels; GPU reports, L2 synchronization, complete pixel/guard and code-hash checks plus retirement are required.
Local packet/image/failure models, sanitizers, real NETB and full ARM64 build pass. **74 native JSON fixtures** and native SYS/CAT/all 11 catalogue memberships pass; fixtures substitute device I/O. The Windows ARM64 NAK compiler now builds reproducibly and runs on this Lite: VS/FS code, headers, metadata and disassembly match accepted host output byte-for-byte. That is CPU compilation, not an ICD or Vulkan draw. Failed .40 trapped `0x1528=0` (Horizon software method); later shader versions omit it and preserve golden fault handling. Details belong in the [module contracts](../../WindowsDrivers/drivers/TegraGpu/README.md) and logs.

## Installed state

- **TegraGpuProbe .46 / ABI 24 / 752 bytes / oem74.inf**, Started/OK. Matching tool: `C:\ProgramData\SwitchWOA\GpuUpdate\gr-backend-0.1.0.46\LitePerformanceCheck.exe` and Public Desktop (unchanged ABI-24 executable). Read-only Desktop status confirms the accepted shader result and zero owned resources.
- GPU-only DSDT: 3,676 bytes, SHA256 `b829b50944a90745fb6f6f7f44f453a6b8573fe4019a95fefd0013a983703518`; adds GPU0 LIC `0x60004414/0x1C`, retaining GSI 189/190 and all other devices. `SWITCH_LITE_GPU_IRQ=1` requires GPU opt-in. Original 3,664-byte DSDT hash: `1bac65cbe8029f40f77bf2ed00ab5808cadcba916dac4325fdc8246bf1302878`. Hash-guarded table rollback: `GpuUpdate\gpu-irq-0.1.0.32\Rollback-GpuIrqTable.ps1`; table changes need reboot.
- **Owner returned the device for GPU tests.** No GPU operation or trace remains active. The final read-only check confirms unchanged GPU table, Never settings and CPU diagnostic 0 / 998.4 MHz; no Hekate/power-policy change was needed.
- Previous GPU package/service/matched-tool backups (.44/.43/.41/.39/.40 and older) remain under local/remote `GpuUpdate` rollback directories. Preserve side chat's uncommitted `WindowsDrivers/drivers/SwitchPowerButton` files; last observed PowerButton is .6/runtime 6, legacy mode/ready 0. Do not stage side files with GPU work.

## Next work

**WIP `3a8d5bc`, library only:** `windows/hardware.c` owns the cold power/clock/firmware arenas, remaps shared command USERD into BAR1 and installs the golden template through an explicit adapter API. `HardwareOwner` pins Commands through partial startup and isolation/free retry. Per-context **976 KiB** GR backing/mappings preserve saved state and reject previous reset generations. The worker now requires GPU-alias fence plus synchronized CPU readback. Full ARM64 build and native hardware/context/paging tests pass with GPU/OS substitutes; complete startup/queue/packed-context execution has no physical WDDM pass. A real I/O provider, worker, paging/IRQ/TDR dispatch and registration remain open. UMD ABI 1 reports the OS LUID and **512 MiB ceiling**; native-submit stays unset.

**ICD prerequisites:** Windows ARM64 NIR/NAK compiler build/native output checks pass, and build-host Mesa CLC/`vtn_bindgen2` tools are ready. The external Mesa checkout is unchanged. Windows `nvkmd` and the full NVK ICD DLL remain unimplemented.

1. Run an actual **NVK Vulkan API triangle with known pixel readback**. Connect the accepted NAK shader/image/render-target path to NVK commands; the private shader diagnostic does not fulfill this gate.
2. Connect the real platform I/O/firmware provider and worker to the implemented hardware lifecycle, then paging/command dispatch, VidMm residency/validation, ISR/DPC completion, preemption and TDR. Validate packed contexts and the USER/KERNEL batch physically while retaining POST display. Publish DRIVERCAPS/DriverEntry only with the required paths connected.
3. Add Windows `nvkmd` discovery/allocation/mapping/submission/waits, build ARM64 ICD DLL/JSON and match the WDDM LUID. Require an actual Vulkan off-screen draw with known foreground/background pixel readback.
4. Add stable landscape HWND presentation with resize/recreation checks, then separately evaluate DWM interop/shared surfaces/fences and compositor GPU work. A CPU-copy GDI route is not GPU-backed presentation.

## Resume and evidence lookup

Keep this entry short; replace stale state instead of appending chronology. Read a specific module/log only as needed. Detailed earlier history: `git show 4a82145:docs/gpu-vulkan/STATUS.md`. Current local evidence root: `/private/tmp/t214-gpu-round`.

- Physical: **`backend46-physical-result.json` + `backend46-physical-run-meta.json`**. Earlier `hot44/boot43/draw41/gr31/irq34/gr36/clear39-physical-result.json`; failed-draw diagnosis `draw40-physical-result.json`.
- Build/sign/install: `backend46-{build,stage,signature-verify,install-result}.log`; native WDK fixtures `backend46-native-contract.log`, sanitizers `backend46-sanitizers.log`; final snapshot `backend46-handoff.log` and `backend46-desktop-status.json`. Package `backend46-package`, remote `GpuUpdate\gr-backend-0.1.0.46`; .44 rollback exported locally/remotely (`backend46-backup-manifest.json`). Table proof: `irq32-acpi/`; unchanged tool fixture: `draw40-native-json.log`.
- WDDM WIP: current ARM64 artifacts in `backend46-build/`; remote controlled tests in `GpuUpdate/wddm-backend46`. Earlier packed-context proof: `graphics45-{build-v2,native-contract-v2}.log`. These are CPU models, not native adapter/ICD evidence.
- Shader compilation: `draw40-shaders/` reproduces the committed shader words; Windows build/native/rebuild evidence is in `nak-win43/{build,repro-build,native-compile,native-repro}.log` and `native*-comparison.json`. `icd-host-tools-{setup,build}.log` records the build-host CLC tools. No ICD DLL or Vulkan API result is implied.
- SSH: `ssh -i ~/.ssh/dsh_handheld_ed25519 ruje@192.168.8.6`. Direct `C:\Users\Public\Desktop\LitePerformanceCheck.exe --gpu-json` only queries. Separate run flags: `--gpu-729`, `--gpu-gr-729`, `--gpu-irq-729`, `--gpu-3d-729`, `--gpu-clear-729`, `--gpu-draw-729`; capture stdout synchronously. Query exit 0 alone is not a GPU pass. SSH timeout alone is not a confirmed hang.

References: [pinned Mesa/NVK](https://github.com/danfromtico/mesa-switch/tree/0ff52079ca9e9e837ed27eb95a8d239f439a1864/src/nouveau/vulkan/nvkmd) · [Switchroot nvgpu](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/tree/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu) · [Linux nouveau v6.12](https://github.com/torvalds/linux/tree/v6.12/drivers/gpu/drm/nouveau/nvkm) · [WDDM sample](https://github.com/microsoft/graphics-driver-samples/tree/de4a2161991eda254013da6c18226f5ea06e4a9c/render-only-sample/roskmd) · [deko3d](https://github.com/devkitPro/deko3d/blob/373bfd43d8b2d805fa2eb7d406dbb823032df958/source/dk_queue.cpp).
