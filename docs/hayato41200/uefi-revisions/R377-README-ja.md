# R377 — DSDT に時計（RTC0）を追加する（R376 ベース）

## 変更点

`Dsdt-R377.asl` に `Device (RTC0)` を追加した。R376 までのデバイスは、そのまま入っている。

| 項目 | 内容 |
|---|---|
| `_HID` | `"SWOA0801"`（`switchrtc.sys` 用） |
| I2cSerialBusV2 | 0x68（MAX77620 の RTC）、400 kHz、`\_SB.I2C5` |

- **UEFI の表示:** R377 にした。
- **変えていないもの:** バックライト（R376）などの動きは、R376 と同じ。UEFI の時計（仮のもの）も、そのまま。時刻は Windows のドライバーが直す。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R377.rom`
- SHA-256: `698d7bbad5bc4405c32c9c11e63c02d3f267527899e4e36ee681a6b238605f45`
- `EDK_SOURCE_RESTORE_MATCH=YES`。iasl の警告は、もとからある 3128 の1件だけ。
- AML を逆アセンブルして、RTC0 を確認した。
- **実機の結果（2026-10-26、ユーザー報告）:** RTC ドライバー v2 と組み合わせて、Windows の時計が正しく合うようになった。

## 手順

`..\Windows-Drivers\Switch-Rtc\README-ja.md` を見てほしい。

## もし起動しなくなったら

R376 の ROM で起動すれば、元に戻る。
