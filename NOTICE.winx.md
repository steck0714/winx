# winx - licensing and third-party notices

> This is a good-faith summary written while preparing the release. It is **not legal advice**.
> Where it matters (e.g. you want to relicense), check the actual file headers and ask a lawyer.

## 1. Effective license of winx

**GPL-2.0-only** (the text is in `LICENSE`, unchanged from upstream).

Why *only* and not "or later": winx contains code ported from **hekate** (CTCaer), whose files say
"version 2, as published by the Free Software Foundation" with no "or any later version". The
upstream project (NintendoSwitchPkg) is also distributed under GPL version 2 (`LICENSE`), and many of
its files are GPL-2.0+ (U-Boot/NVIDIA derived), which can be used under GPL-2.0.

Files that are *entirely* under another, GPL-compatible license keep that license (see table).
Those licenses are permissive and may be combined into a GPL-2.0 distribution; the combined
distribution as a whole is GPL-2.0-only.

## 2. Components

| Component | Where | License | Notes |
|---|---|---|---|
| NintendoSwitchPkg (Ben (Bingxing) Wang / imbushuo, 2019-2021) | whole tree | GPL-2.0 (`LICENSE`); per-file: GPL-2.0+, GPL-2.0, BSD-2-Clause-Patent, old "BSD License" headers (Apple, Linaro, Microsoft, Andrei Warkentin derived), or no header (= repository license) | Upstream: https://github.com/imbushuo/NintendoSwitchPkg, base commit `09c3ade` |
| EDK2 (TianoCore / Intel) | `Drivers/EhciDxeTegra/*` (copy of `MdeModulePkg/Bus/Pci/EhciDxe`) | BSD-2-Clause-Patent | Per-file SPDX headers kept. Text: `LICENSES/BSD-2-Clause-Patent.txt` |
| hekate (CTCaer) | ported into `Drivers/EhciPciEmulationDxe/PciEmulation.c` (PLLU/UTMIPLL/UTMIP PHY init, bm92t36 I2C transfer, VBUS), `Drivers/SdMmcDxe/SdMmc.c` (SDMMC1 power-up, pad calibration, vendor setup), `Drivers/FanDxe/FanDxe.c` (fan/regulator/GPIO) | GPL-2.0-only, Copyright (c) 2018-2024 CTCaer | https://github.com/CTCaer/hekate (checked against master `45cd81e`). Register sequences are ported; those files are therefore GPL-2.0-only as a whole. |
| hekate memory map / L4T contract | `Include/Device/MemoryMap.h` comments, `Tools/Bl33/bl33_stub.S`, `Tools/make-bl33.py` | interface facts read from hekate `bootloader/l4t/l4t.c` | No hekate code copied; `bl33_stub.S` and `make-bl33.py` carry `BSD-2-Clause-Patent` headers. |
| Linux kernel / coreboot | comments only (`bm92t36` extcon behaviour, address maps) | GPL-2.0 | Behaviour was followed and referenced; if you verbatim-copied code from these, it is GPL-2.0 too (compatible). |
| winx modifications | patch `winx-bl33.patch` on top of upstream | GPL-2.0-only (BSD-2-Clause-Patent where the file header says so) | `Tools/test-fan-trace`, `Tools/test-rtc` have no header: GPL-2.0-only. They do **not** include hekate sources (run against a hekate checkout). |

## 3. Things that are NOT in this repository

* **BL31** (Arm Trusted Firmware, BSD-3-Clause, e.g. the one shipped with LineageOS/switchroot) - you supply it.
* **hekate** itself (GPL-2.0) - you supply it.
* **Windows / WinPE / drivers** - nothing from Microsoft is included; you need your own licensed media.

## 4. If you distribute `bl33.bin`

`bl33.bin` bundles GPL-2.0 code with EDK2 (BSD-2-Clause-Patent) modules. When you hand out the
binary you must also offer the **corresponding source**:

* this repository (or `winx-bl33.patch` + upstream commit `09c3ade`),
* EDK2 tag `edk2-stable202402` (+ submodules brotli, mipisyst),
* the exact build command (see `README.winx.md`),
* the `LICENSE` text and the notices above.

`release/SHA256SUMS` records which build the shipped `bl33.bin` is.

## 5. Names and trademarks

"Nintendo", "Nintendo Switch", "Windows" and others are trademarks of their owners. Use them only to say what
the project works with ("for Nintendo Switch"), not as part of a product name. The name **winx** itself is not
a registered mark as far as this summary knows; check before commercial use.

## 6. Copyright

* Upstream parts: see file headers (Bingxing Wang and others).
* Ported parts: CTCaer (hekate).
* winx changes: Copyright (c) 2026 winx steck0714.
