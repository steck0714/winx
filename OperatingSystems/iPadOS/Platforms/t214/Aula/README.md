# Aula / Switch OLED iPadOS profile

| Field | Status |
| --- | --- |
| Product | Switch OLED HEG-001 |
| SoC | Tegra T210B01 / Mariko family |
| Upstream iPadOS project | OLED target; its documented path starts from Hekate |
| WinX UEFI-to-XNU path | Not implemented or physically tested |

Use the upstream project's own Hekate route for its reported OLED result. WinX must not treat its BL33 load map or native loader as a WinX UEFI ABI. A direct UEFI path needs a separately implemented loader and handoff. See the [iPadOS model matrix](../../../../../docs/multiboot/ipados-xnu-ja.md).
