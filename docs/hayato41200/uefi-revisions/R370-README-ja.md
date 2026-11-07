# R370 — DSDT に I2C1 と I2C5 を追加する（R369 ベース）

## 変更点

`Dsdt-R370.asl` に `Device (I2C1)` と `Device (I2C5)` を追加した。R369 の GPI0 と BTNS（ACPI0011）は、そのまま入っている。

| デバイス | `_HID` / `_UID` | MMIO | 割り込み | 速度（L4T） | つながっている主な機器 |
|---|---|---|---|---|---|
| I2C1 | SWOA0201 / 1 | 0x7000C000, 0x100 | GSI 70（SPI 38） | 100 kHz | BM92T 0x18、MAX17050 0x36、TMP451 0x4C、BQ24193 0x6B |
| I2C5 | SWOA0201 / 5 | 0x7000D000, 0x100 | GSI 85（SPI 53） | 400 kHz | MAX77621 0x1B/0x1C、MAX77620 0x3C |

- **I2C1 と I2C5 だけにした理由:** UEFI（PmicDxe）がクロックとリセットを有効にしているのは、この2本だけ。ほかのバス（I2C3 のタッチなど）は、クロックを有効にする仕組みを入れてから追加する。
- **UEFI の表示:** R370 にした。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R370.rom`
- SHA-256: `77d827e7670b2faee3fcea8841e9f22999672ae3d31c89e88eae27640e63d4eb`
- `EDK_SOURCE_RESTORE_MATCH=YES`。iasl の警告は、もとからある 3128 の1件だけ。
- `Dsdt-R370.aml` を逆アセンブルして、I2C1 と I2C5 の定義を確認した。
- **未確認:** 実機での動作。
