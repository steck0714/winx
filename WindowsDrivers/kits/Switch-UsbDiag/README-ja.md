# USB の診断：抜き差しで認識しなくなる原因を調べる

> **2026-10-26:** 抜き差しで認識しなくなる症状は、再起動したら直った（ユーザー報告）。一時的な不具合だった可能性がある。記録はまだ取っていない。また起きたときのために、この診断の道具を残しておく。

## 中身

| ファイル | 内容 |
|---|---|
| `swoadiag.sys` | 読むだけの診断用ドライバー（PnP ではない。SwoaHello と同じ方式）。ソースは `..\SwoaDiag\src\swoadiag.c` |
| `UsbDiag.ps1` | ドライバーを1回だけ読み込んで、読んだ値を表示し、すぐに外す |

## 何を読むか（書き込みは一切しない）

| 場所 | 読むもの | 見たいこと |
|---|---|---|
| xHCI（0x70090000） | USBCMD、USBSTS、各ポートの PORTSC | ポートが接続を検出しているか（CCS）、有効か（PED）、状態（PLS）、速度 |
| FPCI（0x70098000） | メールボックス（CMD、DATA_IN、DATA_OUT、OWNER）、SMI の割り込み | XUSB のファームウェアがホストに要求を出したまま、返事を待っていないか |
| IPFS（0x70099000） | 設定、割り込みのマスク | 割り込みの設定が保たれているか |
| PADCTL（0x7009F000） | パッドの割り当て、ポートの設定、OTG パッド、bias、VBUS/ID | 抜いたときに、設定が変わっていないか |

USB-C の状態（バッテリーのドライバーの記録）と、Windows が見ている USB の機器の一覧も、一緒に表示する。

## 手順

管理者のコマンドプロンプトで、次の3回を順に実行する。ラベルは何でもよい。

1. **USB 機器が認識されているとき:**
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\UsbDiag\UsbDiag.ps1 -Label working
   ```
2. **抜いたあと:** 10 秒ほど待ってから実行する。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\UsbDiag\UsbDiag.ps1 -Label unplugged
   ```
3. **挿し直して、認識しなかったとき:** 10 秒ほど待ってから実行する。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\UsbDiag\UsbDiag.ps1 -Label replugged
   ```

結果は、すべて `C:\SwitchWOA-UsbDiag.txt` に追記される。このファイルを見せてほしい（写真でもよい）。

## 見るところ

- **`port N:` の行:** 挿した機器のポート（今までの記録では5番）が、`CCS` になっているか。
  - **3回目で `CCS` が無い:** 電気的に接続が見えていない。PADCTL 側の問題。
  - **3回目で `CCS` はあるが `PED` が無い:** 接続は見えているが、ポートのリセットが進んでいない。Windows（USBXHCI）、または XUSB のファームウェア側の問題。
- **`mailbox:` の行:** `DATA_OUT` や `OWNER` が 0 でなく、`SMI_INTR` が立ったままなら、ファームウェアが返事を待っている。
