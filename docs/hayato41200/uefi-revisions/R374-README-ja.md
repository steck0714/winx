# R374 — DSDT に電源ボタン（PWRB）を追加する（R373 ベース）

## 変更点

`Dsdt-R374.asl` に `Device (PWRB)` を追加した。R373 までのデバイス（GPI0、BTNS、I2C1、I2C3、I2C5、BAT0、FAN0、TCH0）は、そのまま入っている。

| 項目 | 内容 |
|---|---|
| `_HID` | `"SWOA0601"`（`switchpwrbtn.sys` 用） |
| I2cSerialBusV2 | 0x3C（MAX77620 PMIC）、400 kHz、`\_SB.I2C5` |

UEFI の表示は R374 にした。タッチの電源投入など、UEFI の動きは R373 から変えていない。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R374.rom`
- SHA-256: `e9d13daa28806b4c90813dd5221c73aa0f6ffb0a40362b7544e76dab5e59fe40`
- `EDK_SOURCE_RESTORE_MATCH=YES`。iasl の警告は、もとからある 3128 の1件だけ。
- AML を逆アセンブルして、PWRB を確認した。
- **未確認:** 実機での動作。

## 手順

`..\Windows-Drivers\Switch-PowerButton\README-ja.md` を見てほしい。

## もし起動しなくなったら

R373 の ROM で起動すれば、元に戻る。
