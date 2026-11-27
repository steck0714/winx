# Switch Lite game progress — 2026-11-27

PAL4 (Chinese Paladin 4 / 仙剑奇侠传四) now starts and reaches a game scene
with the companion GM20B WDDM / Mesa NVK stack. Warcraft III reaches a map
and renders through NVK on the Switch Lite. Frame rates still need
optimization; these are experimental results, not a claim of smooth or
fully validated gameplay.

| Game | Observed result | Still open |
| --- | --- | --- |
| PAL4, Chinese voice edition | Owner-confirmed startup, movie playback and game scene after the GPU page-directory compatibility fix. | Slow movies and gameplay; game audio absent although desktop audio works. An explicit 10 FPS DXVK cap was removed, with retesting pending. |
| Warcraft III | Owner-confirmed map entry; the photo shows NVK / GM20B and approximately 12 FPS. | CPU/GPU profiling and frame-rate optimization. |

![Warcraft III running on Switch Lite through NVK, approximately 12 FPS](https://raw.githubusercontent.com/ruje0504-2/mesa-switch-woa/238ae71331a1bea82f1dbe6558aed4743d41b50a/tegra-x1-wddm/docs/game-runs-20261127/warcraft3-switch-lite.jpg)

The original photo is published with the owner's permission. It is a
single observation, not an averaged benchmark. Its roughly 11% GPU-load
reading does not identify the bottleneck. Investigation must separate
Windows x86 translation, DXVK translation and synchronization, NVK/KMD
submission and presentation, and GPU execution. The actual loaded game
DLL hashes and a repeatable scene trace are still needed.

The PAL4 fix retains two page-table levels and accepts the already-supported
`0xfe` leaf kind when Windows carries it on a GPU-physical level-1 directory
update; directory entries remain plain child-table pointers. The real-WDK
before/after test and the signed one-use KMD were checked on the Lite.
The diagnostic candidate has not yet completed daily-baseline promotion.
PAL4's Bink 1.5t movie path is not connected to the project's H.264 NVDEC
path, and successful audio initialization does not establish audible output.

[Detailed results, limitations, image hash and driver evidence](https://github.com/ruje0504-2/mesa-switch-woa/blob/238ae71331a1bea82f1dbe6558aed4743d41b50a/tegra-x1-wddm/docs/game-runs-20261127/README.md).

These are **Switch Lite** results using the companion WinX/Mesa stack.
They do not validate another board, another firmware integration, or the
Windows-Switch OLED game path.
