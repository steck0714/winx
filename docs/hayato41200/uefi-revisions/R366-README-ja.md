# R366 — ファンを一定の速さで回す（R365 ベース）

## 目的

R365 で CPU を 1020 MHz にした。Windows にはファンと温度のドライバーが無いので、UEFI の段階でファンを**一定の速さ**で回しておき、その設定のまま Windows を使えるようにする。

## 参考にしたもの

Hekate の `bdk/thermal/fan.c` と `bdk/power/regulator_5v.c`（CTCaer）。

| 部品 | 内容 |
|---|---|
| PWM | PWM1（`0x7000A010`）。極性が反転していて、レジスタの値は `236 - duty`。`0x100` で停止 |
| 出力ピン | LCD_GPIO2（GPIO PV4）。pinmux の機能 1 = PWM1 にし、GPIO の CNF=0（SFIO） |
| 電源 | 電池側の 5V レギュレーター。EN は GPIO PA5（ピン SATA_LED_ACTIVE、機能 1）を出力 High にする |
| 回転数 | タコメーターは GPIO PS7（ピン CAM1_PWDN）。入力でプルアップ。1回転で2パルス |
| PWM のクロック | `CLK_SOURCE_PWM`=CLK_M、分周 3。**クロックが止まっているか、リセット中のときだけ有効にする**（画面のバックライトなど、動作中の PWM を止めないため） |

## 変更点（`R366StartFan`、CPU のクロック設定の直後）

1. **前の状態を表示する。** PWM のクロックとリセット、`CSR1`、関係するピンの pinmux、PA5・PV4・PS7・PCC4 の GPIO の状態。
2. **PWM のクロックが止まっていれば有効にする。**
3. **タコメーターの入力を設定する。** PS7 を GPIO の入力にし、プルアップする。
4. **5V の電源を入れる。** PA5 を GPIO の出力 High にする。
   - Hekate は T210 で PCC4（USB_VBUS_EN0）も Low にしているが、R366 では**触らずに表示だけする**。USB の VBUS への影響を避けるため。
5. **回し始める。** **duty=76/236（約32%）**。Hekate で 47〜52℃ のときの値。`CSR1 = EN | (160 << 16)` にし、LCD_GPIO2 を PWM1 にする。
6. **回転数を測る。** 1秒待ってから、2秒間測って表示する。`R366 after start fan: duty=.. rpm=..`
7. **状態表示に回転数を追加する。** `USB STATUS #1〜#4` のたびに、ファンの duty と回転数（1秒間の測定）も表示する。温度は R364 から表示している。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R366.rom`（10,485,760 bytes）
- SHA-256: `2069978b40457d0d8d9782d60fc59c7144d53c259eb622cfc68834a86cb8d96b`
- ビルド: 完了。新しい警告は無し。`EDK_SOURCE_RESTORE_MATCH=YES`。
- 差分: `R365-to-R366-UsbPowerAudit.diff`
- **未確認:** 実機での動作のすべて。特に次の3点。
  - ファンが回るか（5V の電源が PA5 だけで足りるか）
  - 回転数の読み取り
  - 画面のバックライトなどへの影響

## 試験の手順

1. R366 で起動する（最初は SD 無しでよい）。ファンの音と、排気口の風を確認する。
2. 次の行を撮影する。
   - `R366 fan before`、`R366 GPIO before`
   - （出れば）`R366 PWM clock enabled`
   - `R366 fan set`
   - `R366 after start fan: duty=.. rpm=..`
   - `USB STATUS` のたびに出る `R366 status fan` と `R364 status(...) temp`
3. うまくいったら、Windows でしばらく使って、熱と音を確認する。

## 読み方

| 結果 | 意味 |
|---|---|
| `rpm` が数千で、音もする | 成功 |
| `rpm=0` で、音もしない | 電源が来ていない可能性がある。`PCC4` の値と `VBUS_EN0` の pinmux を見て、次の版で Hekate と同じく PCC4=Low も試す |
| `rpm=0` だが、音はする | 回転数の読み取りだけが失敗している（PS7 のプルアップなど） |
| 画面が暗くなった、または消えた | PWM の設定がバックライトに影響した。`fan before` の値を報告する |
