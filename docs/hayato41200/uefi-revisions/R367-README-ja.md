# R367 — DSDT に GPIO コントローラー（GPI0）を追加する（R366 ベース）

## 変更点

**`Dsdt-R367.asl` に `Device (GPI0)` を追加した。** R366 のビルドで使ったライブツリーの `Dsdt.asl` に、次を足したもの。

| 項目 | 値 |
|---|---|
| `_HID` | `"SWOA0101"`。自作の Windows ドライバー `tegra210gpio.sys`（GpioClx）が使う ID |
| MMIO | `0x6000D000`、長さ 0x1000 |
| 割り込み | GSI 64、65、66、67、87、119、121、157（GIC SPI 32、33、34、35、55、87、89、125）。バンク 0〜7 の順。レベル、アクティブ High。値は L4T の `gpio@6000d000` の `interrupts` から |
| `_STA` | 0x0F |

その他:
- **UEFI の表示を R367 にした。** UsbPowerAudit の処理は、R366 と同じ（ファン、CPU 1020 MHz、USB、ハブでの充電）。
- **`package-R367.sh` を直した。** DSC や FDF と同じく、DSDT もビルドの間だけ入れ替え、終わったら元に戻して比較する。
- **注意:** ライブツリーの `Dsdt.asl` には GPI0 が入っていない。R368 以降も GPI0 を残すには、`Dsdt-R367.asl` を引き継ぐこと。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R367.rom`（10,485,760 bytes）
- SHA-256: `c9e665562e353c9cac950a033e372571a1727d827a1151dc1d94565a8bd0a49f`
- ビルド: 完了。`EDK_SOURCE_RESTORE_MATCH=YES`。
  - DSDT が作り直されたので、EDK2 の `Trim.py` が出す Python の SyntaxWarning が 12 行増えている。ツールの警告で、出力には影響しない。
- `Dsdt-R367.aml` を `iasl -d` で逆アセンブルして、GPI0 の `_HID`、MMIO、8 本の割り込みが正しいことを確認した。
- `SWOA0101` が FV（ACPI テーブルの FFS と FVMAIN_COMPACT）に入っていることを確認した。
- **未確認:** 実機での動作。

## もし起動しなくなったら

R366 の ROM で起動すれば、GPI0 が無いので `tegra210gpio.sys` は読み込まれない。
