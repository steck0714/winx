# 段階0：自作ドライバーを Switch の Windows で読み込めるようにする

## この PC 側（済み）

- **LLVM 23.1.4** を winget で入れた（`clang-cl`、`lld-link`）。Visual Studio は不要。
- **ビルド用のスクリプト** `..\tools\Build-Driver.ps1`。
  - ARM64 の KMDF ドライバー（KMDF 1.15）を、WDK 10.0.19041 のヘッダーとライブラリでビルドし、テスト用の証明書で署名する。
  - WDK のヘッダーは `/imsvc`（システムヘッダー）として扱い、自分のソースだけを `/W4 /WX` で検査する。
  - `WdfDriverEntry.lib` が必要とする `_guard_check_icall` は、MSVC 以外には無い。そこで、何もしない版を `..\tools\kmdf_support.c` に用意した。
- **テスト用の証明書** `CN=SwitchWOA Test Driver Signing`。この PC の CurrentUser\My にあり、公開鍵を `SwitchWOA-Test.cer` に書き出した。
- **テスト用のドライバー** `SwoaHello.sys`。読み込まれると、自分のサービスのキーに `Parameters\Loaded=1` と KMDF の版を書くだけの、PnP ではないドライバー。
  - 確認したこと: リンクできた。PE の Subsystem は native、インポートは ntoskrnl.exe と WDFLDR.SYS、チェックサムあり。署名は付いている（この PC では証明書を信頼していないので、`verify` は「信頼されていないルート」と出る。これは想定どおり）。

## Switch 側の手順

1. **このフォルダーを Switch の Windows に移す。** 例: `C:\SwitchWOA\Stage0`。USB メモリ経由でも、PC で SD にコピーしてもよい。
2. **準備する。** **管理者の PowerShell** で次を実行する。
   ```powershell
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Stage0\Stage0-SwitchSetup.ps1 -Mode Prepare
   ```
   - 証明書を「信頼されたルート証明機関」と「信頼された発行元」に入れる。
   - `bcdedit /set {current} testsigning on` で、テスト署名モードにする。
3. **Windows を再起動する。** 再起動に失敗したら、電源を切ってペイロードを入れ直してよい。bcdedit の変更は、すでに SD に書かれている。デスクトップの右下に「テスト モード」と表示されれば成功。
4. **テスト用のドライバーを読み込む。**
   ```powershell
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Stage0\Stage0-SwitchSetup.ps1 -Mode Hello
   ```
   `RESULT: PASS ... Loaded=1 KmdfVersion=115` と出れば、段階0は完了。ドライバーは最後に止めて削除するので、後には何も残らない。
5. **結果を共有する。** `C:\SwitchWOA-Stage0.txt` を見せてほしい。

## 結果（2026-10-24）: 段階0は完了

- **テスト署名:** Switch の Windows からは、起動中の BCD を開けなかった（「ボリュームが外部で変更された」）。そこで、PC から `Set-SwitchTestSigning.ps1 -Mode Enable` で SD の EFI パーティションの BCD を直接編集した。BCD は `BCD-backup-<日時>` にバックアップした。設定したのは `{default}`（`partition=F:`、`winload.efi`）。その結果、右下に「Test Mode」と表示された。
- **PowerShell:** Switch の PowerShell 5.1 は x86 で動く（Windows 10 ARM64 には ARM64 版の .NET Framework が無いため）。そのため、bcdedit や pnputil、`drivers\` には `C:\Windows\Sysnative\` を通して触れる。
- **1回目:** `SwoaHello` は `sc start` で **487** になった。lld-link が作ったセクションに `NOT_PAGED` が付いていなかったことなどが原因。`Build-Driver.ps1` を、MSVC と同じセクションの属性にする（`/section:...P`、INIT を `D`、`/tsaware:no`、`/merge` 無し）ように直した。
- **2回目:** **`RESULT: PASS driver loaded, Loaded=1 KmdfVersion=115`**。STATE は 4 RUNNING になり、停止も削除も正常だった。

## うまくいかないとき

- **`bcdedit` が「Secure Boot」で拒否される:** UEFI で Secure Boot が有効になっている。`-Mode Status` の `SecureBoot:` の行を見せてほしい。
- **`sc start` が 577 や 1275 になる:** 署名を検証できていない。テスト署名モードになっているか、証明書がストアに入っているかを `-Mode Status` で確認する。
- **テストモードをやめる:** `bcdedit /set {current} testsigning off` を実行して、再起動する。
