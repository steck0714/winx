# R375 — DSDT に CPU のクロック制御（CPUC）を追加する（R374 ベース）

## 変更点

`Dsdt-R375.asl` に `Device (CPUC)` を追加した。R374 までのデバイスは、そのまま入っている。

| 項目 | 内容 |
|---|---|
| `_HID` | `"SWOA0701"`（`switchcpu.sys` 用） |
| Memory32Fixed | 0x60006000、0x1000（CAR。ドライバーが触るのは CCLK_BURST_POLICY、PLLX_BASE、PLLX_MISC だけ） |
| I2cSerialBusV2 | 0x1B（MAX77621、CPU の電源）、400 kHz、`\_SB.I2C5`、Exclusive |
| I2cSerialBusV2 | 0x4C（TMP451）、100 kHz、`\_SB.I2C1`、Shared（ファンのドライバーと共用） |

- CAR の範囲は、ほかのデバイスと重なっていない（GPI0 は 0x6000D000）。
- UEFI の表示は R375 にした。起動時の CPU のクロック（1017.6 MHz、1025 mV）など、UEFI の動きは R374 から変えていない。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R375.rom`
- SHA-256: `5320e758f9c98e4620dd24610ac0a4393cfdfc31d25b436dab44dbeaa6c1e1cd`
- `EDK_SOURCE_RESTORE_MATCH=YES`。iasl の警告は、もとからある 3128 の1件だけ。
- AML を逆アセンブルして、CPUC を確認した。
- **未確認:** 実機での動作。

## 手順

`..\Windows-Drivers\Switch-CpuClock\README-ja.md` を見てほしい。

## もし起動しなくなったら

R374 の ROM で起動すれば、元に戻る。
