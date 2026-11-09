# Switch hardware and SoC source layout

The source tree keeps the four Switch product boards separate, even when they share a SoC family. SoC-level implementation can be reused; board power, GPIO, display, storage and radio wiring must still be selected and validated per model.

The board names and many register/resource constants below are source-derived. The current profile builds are **build verified only**; no current-build physical board-identification test is recorded. Apply the [verification labels](../../docs/verification-status.md) before treating a source constant as a measured value.

| SoC directory | Board directory | Product model | SoC identity | WinX selector |
| --- | --- | --- | --- | --- |
| `t210/Icosa/` | Icosa | Standard Switch, HAC-001 (Erista) | Tegra X1, T210 | `SWITCH_BOARD_ERISTA` |
| `t214/Iowa/` | Iowa | Battery-enhanced Switch, HAC-001-01 (Mariko) | T210B01, also called T214 | `SWITCH_BOARD_MARIKO` |
| `t214/Hoag/` | Hoag | Switch Lite, HDH-001 | T210B01 / T214 | `SWITCH_LITE_TOUCH` |
| `t214/Aula/` | Aula | Switch OLED, HEG-001 | T210B01 / T214 | `SWITCH_BOARD_OLED` |

`t214` is retained as the familiar model-family label; Hekate calls this hardware revision T210B01. Iowa, Hoag and Aula each have their own directory and compile-time profile, and none inherits another model's device setup by default. The canonical profile path is `Platforms/Switch/<SoC>/<board>/BoardProfile.h`; descriptive product aliases are build flags only and do not create parallel source folders.

Current board-specific files are deliberately sparse. Icosa Wi-Fi power remains observe-only. Iowa and Aula have identification profiles but no validated Wi-Fi bring-up. Hoag has Lite-specific display/SIO ACPI and a T214 Wi-Fi supply sequence, which still requires target card profile and real-device validation.

Shared Windows ACPI additions live under `OperatingSystems/Windows/Acpi/`; Hoag-specific GPU/display/SIO ACPI lives under `t214/Hoag/Acpi/`. OS boot fragments are grouped under `OperatingSystems/{Windows,Linux,Other}/Boot/`.

Research references: [Hekate hardware-type definitions](https://github.com/CTCaer/hekate/blob/master/bdk/soc/fuse.h) lists Icosa, Iowa, Hoag and Aula, and [Hekate hardware initialization](https://github.com/CTCaer/hekate/blob/master/bdk/soc/hw_init.c) keeps Hoag-specific initialization distinct. [Switchroot's T210 platform/kernel](https://github.com/CTCaer/switch-l4t-platform-t210-nx) and [Switch-Linux device trees](https://github.com/shinyquagsire23/Switch-Linux) are source references for theoretical board resources, not proof of WinX runtime support. Forum reports are attributed to the named project and contributor; reports about a similar project are not counted as WinX results.
