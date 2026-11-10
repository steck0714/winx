# Windows drivers by Tegra and board

Windows driver code and packages are kept under the Switch SoC and board code name. Status labels distinguish physical reports from source-only work.

| SoC / board | Directory | Current coverage |
|---|---|---|
| T210 / Icosa / Erista | [`t210/Icosa`](t210/Icosa/README.md) | An opt-in BCM4356 Wi-Fi hand-off and INF have been added. Windows runtime and radio power-up are **not verified on Erista**. |
| T214 / Hoag / Switch Lite | [`t214/Hoag`](t214/Hoag/README.md) | The driver README records owner-reported physical Lite results. It also lists hardware tests that are still pending. |

The Windows Wi-Fi packet core is shared from [`t214/Hoag/drivers/SwitchWifi`](t214/Hoag/drivers/SwitchWifi/README.md). The T210 package keeps its ACPI ID and board-specific preparation separate. It does not include Broadcom firmware or calibration data; those must come from the matching board installation.

Related reference code comes from [CTCaer Switch L4T](https://github.com/CTCaer/switch-l4t-kernel-4.9), including its Linux BCM4356 and Tegra PCIe paths. Linux support is design evidence, not proof that the Windows driver works.
