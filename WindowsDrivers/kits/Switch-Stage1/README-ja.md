# 段階1：Tegra210 GPIO コントローラーのドライバー

## 中身

| ファイル | 内容 |
|---|---|
| `tegra210gpio.sys` | GpioClx のクライアント（KMDF 1.15、ARM64、テスト署名）。ソースは `..\Tegra210Gpio\src\tegra210gpio.c` |
| `tegra210gpio.inf` | `ACPI\SWOA0101` に対応する INF |
| `tegra210gpio.cat` | INF のカタログ。テスト署名済み |
| `Stage1-Gpio.ps1` | Switch で実行するスクリプト（Install、Status、Remove） |

### ドライバーの作り

- **バンク:** 8 バンク × 32 ピン（Tegra のバンク = 4 ポート × 8 ピン）。GPIO 番号 = ポート × 8 + ビット。
- **読み書き:** マスク付きの書き込み（+0x80）で、CNF、OE、OUT、INT_ENB を、ほかのピンに触れずに変える。
- **割り込みの種類:** INT_LVL の3つのビット（LVL、EDGE、DELTA）で、レベル High/Low、立ち上がり、立ち下がり、両エッジを設定する（Linux の `gpio-tegra` と同じ）。
- **割り込みとバンクの対応:** バンク n は n 番目の割り込みを使う（`QueryBankInterruptBindingInformation`）。
- **プルアップ/ダウン:** pinmux の側にあるので触らない（ファームウェアの設定のまま）。
- **ビルド:** `/W4 /WX` で警告なし。GpioClx の結合情報（`_GPIOClx_BIND_INFO`）が `.data` に入っていることを確認した。

## 手順

1. **Switch を UEFI R367 で起動する。** ペイロードは `D:\Codex\coreboot-R367.rom`。
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\Stage1`。
3. **管理者のコマンドプロンプトでインストールする。**
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Stage1\Stage1-Gpio.ps1 -Mode Install
   ```
4. **結果を確かめる。** `GPI0 device: ACPI\SWOA0101\0 ... status=OK problem=0 service=tegra210gpio` と出れば成功。デバイスマネージャーの「システム デバイス」にも「NVIDIA Tegra210 GPIO Controller」が出る。
5. **結果を共有する。** `C:\SwitchWOA-Stage1.txt` を見せてほしい。

## 結果の見方

| 表示 | 意味 |
|---|---|
| `GPI0 device: NOT FOUND` | R367 で起動していない（DSDT に GPI0 が無い） |
| `problem=28` | ドライバーが入っていない。pnputil の出力を確認する |
| `problem=10` または `31` | ドライバーが起動に失敗した。イベントログの System を確認する |
| 起動中にブルースクリーン、または再起動を繰り返す | R366 の ROM で起動すれば、GPI0 が無くなるのでドライバーは読み込まれない。その状態で `-Mode Remove` を実行する |

## 結果（2026-10-25）: 段階1は完了

- **v1（1.0.0.0）:** Code 10（STATUS_UNSUCCESSFUL）。バンクと割り込みの対応表（`ResourceMapping`）に、割り込みだけを数えた番号（0〜7）を返していたのが原因。
- **v2（1.0.0.2）:** 対応表を、**リソースの一覧全体での番号**（メモリが 0、割り込みが 1〜8）にした。結果は **`status=OK problem=0`**、「This device is working properly.」。
- **v2 の起動の記録（driver trace）:** すべて想定どおりだった。
  - リソース: `PrepareResourceCount=9`。`Res0Type=3`（メモリ）、`Res1〜8Type=2`（割り込み）、`MmioLength=0x1000`、`PrepareStatus=0`
  - 対応表の問い合わせ: `QuerySetType=1`（バンクと割り込みの対応）、`TotalBanks=8`、`BoundBanks=8`、`Bank0Index=1`
  - 起動: `StartCalled=1`

## まだ確認できないこと

GPIO を使う機器（ボタン）は、段階2で DSDT に追加する。段階1で確認できるのは、「ドライバーが読み込まれて、デバイスが正常に動作中になる」ところまで。割り込みの動作は、段階2で確認する。
