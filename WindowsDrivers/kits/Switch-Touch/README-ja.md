# タッチパネル：Nintendo Switch のタッチスクリーンドライバー

## 中身

| ファイル | 内容 |
|---|---|
| `switchtouch.sys` | KMDF ドライバー（ARM64、テスト署名）。ソースは `..\SwitchTouch\src\switchtouch.c` |
| `switchtouch.inf` | `ACPI\SWOA0501`。下に Windows 標準の `vhf.sys`（Virtual HID Framework）を付ける |
| `Touch.ps1` | Install、Status、Watch、Calibrate、Remove |

## ドライバーの作り

- **使うリソース（DSDT の `TCH0`）:**
  - I2C3 上の STM FTM4（0x49）
  - 割り込み PX1（GPIO 185、Low で発生）
- **前提:** 次のことは UEFI R373 がすでにやっている。ドライバーはイベントを読むだけ。
  - タッチパネルの電源を入れる（LDO6 2.9 V、リセット PJ7）。
  - コントローラーをリセットして、指の検出をオンにする。
- **起動時:**
  1. チップ ID（3670）を確かめる。
  2. たまったイベントを消す。
  3. コントローラーの割り込みを有効にする（HW レジスタ 0x2C に 0x41）。
- **読み方:** 割り込みが来たら、イベントの数を読み、8 バイトずつ読む。
  - 割り込みが一度も来ないときは、12 ms ごとのポーリングで読む。
  - 割り込みが来ていれば、確認のポーリングは 250 ms ごとに減らす。
- **Windows への渡し方:** HID のマルチタッチスクリーン（10 本指）として報告する。
  - 画面は縦長（720×1280）として扱う。
  - 向きが合わないときは `-Mode Calibrate` で直す。
- **記録:** `Services\switchtouch\Parameters` に、次の値を書く。
  - ChipId、Irqs、Polls、Events、Reports、Errors、LastLandscapeX/Y など

## 手順

1. **Switch を UEFI R373 で起動する。** ペイロードは `D:\Codex\coreboot-R373.rom`。
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\Touch`。
3. **インストールする。**
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Touch\Touch.ps1 -Mode Install
   ```
   - `device: ACPI\SWOA0501 … problem=0` になっているか見る。
   - `HID child:` に「HID 準拠タッチ スクリーン」が出ているか見る。
4. **画面を触ってみる。** カーソルが動くか、タップでクリックになるかを見る。
5. **触った場所とずれていたら、向きを直す。** 次を実行して、20 秒以内に、今見えている画面の**左上の角**を指で押さえる。設定を変えて、デバイスを再起動する。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Touch\Touch.ps1 -Mode Calibrate
   ```
6. **結果を共有する。** `-Mode Status` の出力を見せてほしい。特に見たいのは次の2つ。
   - `Irqs` が増えているか（割り込みが動いているか）。
   - `Errors` が 0 のままか。

## 結果（2026-10-25、ユーザー報告）

- **v1:** 触ると反応した。左右は合っていたが、上下が逆だった（上を触ると下が押される）。
- **v2（1.0.0.2）:** `CfgInvertY` の既定値を 1 にした。
  - ただし、v1 を入れた Switch のレジストリには `CfgInvertY=0` がすでに書かれている。そのため、`reg add` で 1 に変える必要がある。

- **5本指くらいまでのマルチタッチと、スクロールが動いた。**
- **割り込みも動いている。** Irqs が増えていたので、割り込みを有効にするレジスタ（0x2C に 0x41）の推測は合っていた。
- **v3:** 長押しでも右クリックにならなかった。
- **v4（1.0.0.4）:** 長押しで右クリックが出るようになった。直したのは次の2つ。
  - 指を置いてから約 1.2 mm 動くまでは、最初の位置のまま報告する。
  - 指が触れている間は、報告を続けて送る。

## うまくいかないとき

- **`vhf service: NOT REGISTERED`:** この Windows に vhf が入っていない。その表示を見せてほしい。
- **Code 10:** `driver trace` の `OpenI2cStatus`、`ChipInfoStatus`、`ChipId` を見せてほしい。
- **触っても反応しない:** `-Mode Watch` を実行して画面を触り、`events` と `reports` が増えるか見てほしい。
- **元に戻したいとき:** `-Mode Remove` でドライバーを外す。R372 の ROM で起動すれば、TCH0 自体が無くなる。
