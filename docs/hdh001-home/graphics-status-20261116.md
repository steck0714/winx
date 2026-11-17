# Switch Lite downstream graphics status — 2026-11-16

**Experimental and still unstable. High load can readily cause system hangs,
crashes or unexpected reboots on the tested Switch Lite. The functional
milestones below do not establish reliable daily use or sustained-load
stability.**

This is a status report for the downstream Windows ARM64 port on Switch Lite
(HDH-001 / T214). The firmware support previously described in WinX PRs
[#6](https://github.com/steck0714/winx/pull/6) and
[#7](https://github.com/steck0714/winx/pull/7) has been followed by experimental
desktop GPU acceleration in the companion Mesa/WDDM repository. These results
apply to that specific downstream component combination; merging this document
does not install or add the graphics implementation to WinX.

## Fixed checkpoint and architecture

The last recorded milestone before NVDEC integration is
[`da1bda2bb0fc5c825f298c8f35a37ac737bf5d30`](https://github.com/ruje0504-2/mesa-switch-woa/commit/da1bda2bb0fc5c825f298c8f35a37ac737bf5d30)
on `tgd3d-vulkan`. It records fixed 90° landscape with normal row scanout and
native Win32 `vkcube` validation.

The desktop path is Windows D3D/DXGI runtime → tgd3d → Vulkan/NVK → WDDM KMD →
GM20B/display controller. GPU sources remain in the companion Mesa/WDDM
repository. The active development branch contains later work with a different
validation status; the links in this report are pinned to the checkpoint.

## What was observed

- Ordinary DWM loaded the recorded tgd3d UMD and NVK ICD. Window movement,
  resizing, minimizing/restoring, occlusion recovery and closing were exercised.
- Fixed 90° handheld landscape uses a logical 1280×720 desktop with GPU
  pre-rotation. The physical panel remains 720×1280 with normal row scanout,
  pitch 2880 and Tegra DC column scanning disabled.
- Native ARM64 Win32 `vkcube` used the NVK adapter with a LUID matching the KMD
  within the test boot. Rendering and copying use NVK; the current Win32 WSI
  path passes the image through a GDI window to DWM for composition.
- The timed run lasted **300.289 seconds**. Across the full recorded run,
  **30 swapchain creations/recreations** and **7,769 Present calls** returned
  success. The owner confirmed the rotating cube, movement/resizing and a
  separate occlusion/recovery check.
- The timed interval averaged **19.77 FPS in Present-return cadence**, including
  manual resizing and maximizing. This is an interactive-session measurement,
  not a fixed-size performance benchmark or a measurement of panel flip
  completion.

The cube requested Vulkan 1.0 with Win32 surface and swapchain extensions.
This test does not establish Vulkan conformance, full D3D application
compatibility or DXVK game compatibility.

## Component identity and evidence

| Component | SHA256 of the tested binary |
| --- | --- |
| `tegragpurender.sys` | `e95287cbeb1d2c325baacb9685affd1d24783b22ecadc1801ba1dffcfe7cc25e` |
| `TegraGpuD3DVk.dll` | `5b66bbec86af299941812e8376cc1ac4a5630385828c895b7a52ec96d5c5ad7a` |
| `libvulkan_nouveau.dll` | `95b97df4e83a2a90315a8784d83e75771021093dec34f98362b584d1f546bdff` |

The [native cube report](https://github.com/ruje0504-2/mesa-switch-woa/blob/da1bda2bb0fc5c825f298c8f35a37ac737bf5d30/tegra-x1-wddm/docs/VKCUBE_ACCEPTANCE_20261116.md),
[display report](https://github.com/ruje0504-2/mesa-switch-woa/blob/da1bda2bb0fc5c825f298c8f35a37ac737bf5d30/tegra-x1-wddm/docs/ROW_SCAN_BASELINE_20261116.md) and
[machine-readable baseline](https://github.com/ruje0504-2/mesa-switch-woa/blob/da1bda2bb0fc5c825f298c8f35a37ac737bf5d30/tegra-x1-wddm/docs/ROW_SCAN_BASELINE_20261116.json)
record source inputs, component hashes, observation durations, owner feedback
and cleanup. Hardware results attach to those recorded binaries and source
inputs, including separately tracked platform patches; the checkpoint is not
a claim that an arbitrary rebuild or later branch head has identical validation.

## Open stability problems

- **High-load hangs/crashes and unexpected reboots remain unresolved.** One
  reported case occurred on battery with CPU-Z and vkpeak open, when vkpeak was
  closed. Powering the device externally has sometimes helped, but does not
  establish a fix.
- Reboots have also been reported while opening the weather/widgets UI or a
  browser. The failures have not been isolated to GPU load, and a shared cause
  has not been established.
- Returning from **180° to 90° caused an unexpected reboot**. Short successful
  images in other orientations do not establish reliable four-direction
  switching.
- DC column scanning, long-duration stability and sustained-load reliability
  remain outside the accepted fixed-90° milestone. SC7 remains disabled.

The limited desktop/cube runs cannot override these known failures.

## Work after this checkpoint

NVDEC/D3D11VA integration is ongoing separately. **Neither the complete 720p
nor 1080p hardware-decode-to-presentation path has passed acceptance.** The
sequence is 720p H.264 first, then 1080p. Separate higher-resolution work remains
untested and its integration is undecided. Source availability or build success
must not be reported as working hardware decoding.

DXVK integration and broader application compatibility remain separate work.
Independent review of the added timing/observation helpers is still pending.
This documentation update adds no drivers, binaries, firmware changes or new
hardware-test claims.
