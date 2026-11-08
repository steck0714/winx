# R371 — DSDT にバッテリー（BAT0）を追加する（R370 ベース）

## 変更点

`Dsdt-R371.asl` に `Device (BAT0)` を追加した。R370 の GPI0、BTNS、I2C1、I2C5 はそのまま入っている。

| 項目 | 内容 |
|---|---|
| `_HID` | `"SWOA0301"`（`switchbattery.sys` 用） |
| I2cSerialBusV2 0 | 0x36（MAX17050 の残量計）、100 kHz、`\_SB.I2C1` |
| I2cSerialBusV2 1 | 0x6B（BQ24193 の充電 IC）、100 kHz、`\_SB.I2C1` |

UEFI の表示は R371 にした。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R371.rom`
- SHA-256: `103ffc1a5e691f2a26bf11efd48c6b787019fc3859ce139247bf6cd630f0f7f2`
- `EDK_SOURCE_RESTORE_MATCH=YES`。iasl の警告は、もとからある 3128 の1件だけ。
- `Dsdt-R371.aml` を逆アセンブルして、BAT0 の I2cSerialBusV2 が2つ（`"\\_SB.I2C1"`）あることを確認した。
- **未確認:** 実機での動作。
