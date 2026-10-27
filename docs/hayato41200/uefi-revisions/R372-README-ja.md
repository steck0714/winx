# R372 — DSDT にファン制御（FAN0）を追加する（R371 ベース）

## 変更点

`Dsdt-R372.asl` に `Device (FAN0)` を追加した。R371 までの GPI0、BTNS、I2C1、I2C5、BAT0 は、そのまま入っている。

| 項目 | 内容 |
|---|---|
| `_HID` | `"SWOA0401"`（`switchfan.sys` 用） |
| Memory32Fixed | 0x7000A000、0x100（PWM コントローラー。ドライバーが触るのは CSR_1 だけ） |
| I2cSerialBusV2 | 0x4C（TMP451）、100 kHz、`\_SB.I2C1` |

UEFI の表示は R372 にした。ファンの起動（PA5、PWM1、32%）は R366 から変えていない。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R372.rom`
- SHA-256: `b37e7d3cff507091297fe72c1c541014b3f9b10165e485b1bee525289c6b8482`
- `EDK_SOURCE_RESTORE_MATCH=YES`。iasl の警告は、もとからある 3128 の1件だけ。
- AML を逆アセンブルして、FAN0 を確認した。
- **未確認:** 実機での動作。
