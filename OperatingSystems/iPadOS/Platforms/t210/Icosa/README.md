# Icosa / Erista iPadOS profile

| Field | Status |
| --- | --- |
| Product | Switch HAC-001 |
| SoC | Tegra T210 / Erista |
| Upstream iPadOS project | Not in its OLED-only support statement |
| WinX UEFI-to-XNU path | Not implemented or physically tested |

This board needs a T210-specific Hekate/BL31 and memory handoff, Apple Device Tree, and XNU drivers. Do not copy Aula or T210B01 values into this profile. Resource values require a cited source and remain theoretical until physical readback. See the [iPadOS model matrix](../../../../../docs/multiboot/ipados-xnu-ja.md).
