# Bluetooth：Nintendo Switch の Bluetooth ドライバー

**動作確認（1.0.0.13、2026-10-31）:** キーボード、オーディオ（A2DP）、再起動・シャットダウンのあとも使える。電波は機器にかなり近づけないと届かない（アンテナまわりは Wi-Fi 側が動いていないため。Wi-Fi のドライバーのときに見る）。

チップを、Windows に最初から入っている Bluetooth の部品（BthMini）につなぎます。うまくいけば、設定に「Bluetooth」が出て、マウスやキーボードとペアリングできます。

## 中身

| ファイル | 内容 |
|---|---|
| `switchbt.sys` | KMDF ドライバー（ARM64、テスト署名）。ソースは `drivers\SwitchBluetooth\src\switchbt.c` |
| `switchbt.inf` | `ACPI\SWOA0B01` |
| `Bluetooth.ps1` | Install、Status、Remove |

## ドライバーの作り

- **使うリソース（DSDT の `BTH0`、UEFI R390 以降）:**
  - UART-D（0x70006300）。Wi-Fi と一体の BCM4356A3 の Bluetooth が、ここにつながっている（H4）。
  - 割り込み GSI 122。
- **前提:** UEFI R390 が、UART-D のピンとクロックを設定し、チップの電源（PH4）を入れる。
- **起動したとき:** 115200 bps で次を送り、答えを `Services\switchbt\Parameters` に書く。
  - HCI_Reset、Read_Local_Version
  - Write_BD_ADDR：チップのアドレスは仮の `AA:AA:AA:AA:AA:AA` なので、Switch ごとに1つ作って（98:B6:E9 + ランダムな3バイト）、レジストリの `AddressSuffix` に覚えておく。毎回同じアドレスを書く。
  - Read_BD_ADDR、Read_Local_Name
- **そのあと:** 子デバイス `SWOA\BTH_UART_H4`（互換 ID `MS_BTHX_BTHMINI`）を作る。Windows の `bth.inf` が、そこに BthMini と Bluetooth のスタックを入れる。
  - 送受信は割り込みで行う。速度は 115200 bps のまま（マウスやキーボードには十分。音声は第3段）。
  - ファームウェアのパッチは、まだ当てない（チップの ROM のまま）。

## 第3段：ファームウェアと 3 Mbps（1.0.0.9 以降、UEFI R390 でよい）

- **ファームウェアのパッチ:** Broadcom の `BCM4356A3.hcd` が要る。Broadcom のファイルなので、ここでは配らない。自分で持っているもの（Switchroot の Linux の `/lib/firmware/brcm/BCM4356A3.hcd` など）を使う。
  ```bat
  powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Bluetooth\Bluetooth.ps1 -Mode Firmware -Path <BCM4356A3.hcd の場所>
  ```
  - `System32\drivers\SwitchWOA\BCM4356A3.hcd` にコピーする。形（Broadcom のコマンドの並び）を確かめてからコピーする。
  - 次にドライバーが始まるとき（再起動）、HCI_Reset、Download_Minidriver（FC2E）、ファイルの中身、HCI_Reset の順に送る（Linux の btbcm と同じ）。115200 bps で数秒かかる。
- **3 Mbps:** ドライバーが UART-D のクロックを自分で切り替える（Linux と同じ）。始めは PLLP 408 MHz・分周 221 で 115200 bps。Broadcom のコマンド（FC18）でチップを 3 Mbps にしたら、CLK_SOURCE_UARTD を 48 MHz（bit 24 の上書き、N = 15）にして合わせる（フロー制御あり、だめならなし）。
  - CLK_SOURCE_UARTD の bit 24 は、UART 自身の分周（DLL/DLM）を使わず、CAR のクロックをそのまま 16 倍のクロックにする切り替え。R391 はこれを 48 MHz にしたので、UART が 3 Mbps になり、UEFI の 115200 bps の確認に答えがなかった。R391 は使わず、R390 でよい。
  - `Parameters\HighSpeed` を 0 にすると、115200 bps のまま。
- `-Mode Status` の `stage 3 (1.0.0.8+)` の行に、UART のクロック、速度、ファームウェアの結果が出る。

## 起動が止まったとき（1.0.0.5 以降）

- ドライバーは、起動のたびに `BootPending` をディスクに書き、子デバイスを作ってから 10 秒（起動から約 30 秒）動いたら消す（1.0.0.12 までは 60 秒）。
- 前の起動で消えていなければ（Windows が途中で止まった）、その起動ではドライバーは何もしない（`SafeMode=1`）。そのまま Windows は起動できる。
- `Stage`（1 追加、2 キュー、3 ハードウェア、4 チップの確認、5 動作中、6 子デバイス追加。1.0.0.6 から子デバイスは起動の20秒後）と、やり取りの数も、その場でディスクに書く。止まった起動で、どこまで進んだかが分かる。
- もう一度動かすときは `-Mode Retry`（または `-Mode Install`）を実行する。1.0.0.13 からは、数秒でそのまま動き出す（再起動は要らない）。
- Windows の「シャットダウン」は、高速スタートアップでは休止状態に近く、ドライバーは読み込み直されない。1.0.0.12 までは、Retry のあとに再起動が要った。

## 手順

1. **Switch を UEFI R390 で起動する。**
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\Bluetooth`（第1段のフォルダーは上書きしてよい）。
3. **インストールする。** 管理者の PowerShell で実行する。第1段が入っていても、そのまま上書きでよい。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Bluetooth\Bluetooth.ps1 -Mode Install
   ```
   - `RESULT: the Bluetooth chip answers.` と、`BdAddr=98:B6:E9:...` が出る。
   - `radio: SWOA\BTH_UART_H4 ... problem=0` と `RESULT 2` が出れば、Windows の Bluetooth が動いている。
4. **設定 → Bluetooth とデバイス** を開いて、Bluetooth のスイッチが出るか、機器を探せるか試す。

うまくいかないときは、`-Mode Status` の結果（`C:\SwitchWOA-Bluetooth.txt`）を見せてください。
