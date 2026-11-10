# 段階3：Tegra210 I2C コントローラーのドライバー（SpbCx）

## 中身

| ファイル | 内容 |
|---|---|
| `tegra210i2c.sys` | SpbCx のコントローラー（KMDF 1.15、ARM64、テスト署名）。ソースは `..\Tegra210I2c\src\tegra210i2c.c` |
| `tegra210i2c.inf` | `ACPI\SWOA0201` に対応する INF。1つのドライバーで I2C1 と I2C5 の両方を扱う |
| `Stage3-I2c.ps1` | Switch で実行するスクリプト（Install、Status、Remove） |

## ドライバーの作り（v1）

- **転送は、ポーリングのパケットモード。** UEFI の R288 の関数や Linux の i2c-tegra と同じ方法。
  1. メッセージ1つを、1パケットとして送る（ヘッダー 0x10010、サイズ−1、アドレス<<1 と READ・REPEAT_START）。
  2. FIFO の空き具合を見ながら、データを書き込む（または読み出す）。
  3. `PACKET_XFER_COMPLETE` を待つ。
- **エラーの扱い:**
  - NACK → `STATUS_NO_SUCH_DEVICE`（0xC000000E）
  - アービトレーションの失敗 → バスクリア（9 クロック）をして `STATUS_IO_DEVICE_ERROR`
  - タイムアウト → バスクリアをして `STATUS_IO_TIMEOUT`（0xC00000B5）
- **SPB の要求:** Read、Write、Sequence に対応する。Sequence の転送どうしの間は、REPEAT_START でつなぐ。7 ビットアドレスのみ。
- **バスの速度:** UEFI が設定した CLK_DIVISOR のまま使う。ACPI の接続速度は、記録するだけ。
- **自己テスト（読み取りだけ）:** 起動時（D0Entry）に、既知のチップのレジスタを1つずつ読む。
  - I2C1（0x7000C000）: TMP451 のローカルとリモートの温度、BQ24193 の REV（0x2F のはず）
  - I2C5（0x7000D000）: MAX77620 の ONOFFSTAT、MAX77621 の VOUT（R365 以降なら 0xC3 のはず）
- **記録:** 結果、転送の回数、エラーの回数を `Services\tegra210i2c\Parameters` に `I2C_C000_...`、`I2C_D000_...` の名前で書く（2秒ごと）。

## 手順

1. **Switch を UEFI R370 で起動する。** ペイロードは `D:\Codex\coreboot-R370.rom`。R370 には、GPIO とボタンも入っている。
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\Stage3`。
3. **インストールする。** 管理者のコマンドプロンプトで、次を実行する。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Stage3\Stage3-I2c.ps1 -Mode Install
   ```
4. **結果を確かめる。**
   - `I2C device: ACPI\SWOA0201\1` と `...\5` が、どちらも `status=OK problem=0` になれば成功。
   - `trace I2C@7000C000: ... SelfTest_BQ24193_Rev=0x12F` のように、`0x1XX` の値が出れば、実際に I2C で読めている。
5. **結果を共有する。** `C:\SwitchWOA-Stage3.txt`（または写真）を見せてほしい。

## v4（1.0.0.4、2026-10-29）: 診断用のタイマーを外した

- 2秒ごとに、転送やエラーの回数をレジストリに書いていたタイマーを外した。
- エラーの回数（Errors、Nacks、Timeouts、LastErrorStatus、LastErrorAddr）は、エラーが起きたときにだけ書く。
- 接続のたびに書いていた調査用の記録（ConnectRaw0〜7 など）を外した。接続できなかったときだけ、理由を書く。
- 動きは v3 と同じ。

## 結果（2026-10-25）: 段階3は完了

インストールで `ACPI\SWOA0201\1` と `\5` の両方にドライバーが入り、**両方とも `status=OK problem=0`** になった。

| バス | 項目 | 値 | 意味 |
|---|---|---|---|
| I2C1（7000C000） | ResourceCount | 2 | メモリと割り込み |
| | CnfgBefore → After | 0x20800 → パケットモード | |
| | ClkDivisor | 0x50001 | |
| | SelfTest_TMP451_Local | **0x124** | ボード 36℃ |
| | SelfTest_TMP451_Remote | **0x126** | SoC 38℃ |
| | SelfTest_BQ24193_Rev | **0x12F** | REV=0x2F（想定どおり） |
| I2C5（7000D000） | CnfgBefore | 0x2840 | |
| | ClkDivisor | 0x50001 | |
| | SelfTest_MAX77620_OnOffStat | **0x102** | ONOFFSTAT=0x02 |
| | SelfTest_MAX77621_Vout | **0x1C3** | VOUT=0xC3 = 1025 mV（R365 で設定した値） |

**[事実]** Windows の自作 I2C ドライバーから、両方のバスで実際にチップを読めた。値も UEFI 側の設定や温度と矛盾しない。

## もし起動しなくなったら

R369 の ROM で起動すれば、I2C のデバイスが無い状態に戻る。その状態で `-Mode Remove` を実行する。
