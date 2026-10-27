# WinPE (ARM64) USB テスト計画(テスト用・未検証)

## このキットの中身
| ファイル | 用途 |
|---|---|
| `bl33/bl33_wintest_A_ehci.bin` | **A 案。** あなたの統合版(`bl33.bin`)に、**ACPI の EHCI(`NVDA0214`、`_CID` は `PNP0D20`)だけ**を足したものです。標準の usbehci が担当します。UEFI の動作は変わりません。 |
| `bl33/bl33_wintest_B_urs.bin` | **B 案(実験)。** 同じコントローラを、Windows 標準の **USB ロールスイッチ(URS)と ChipIdea 用ドライバ**に任せる記述にしたものです(`URS0`、`_HID` は `PNP0C90`)。リセット後のホストモードを、ACPI の `_UBF` が書き戻します。 |
| `uefi/ehci_reset_probe.nsh` | Windows の標準ドライバが起動時にすることを真似て、**HCRESET の後に USBMODE がホストのまま残るか**を、UEFI シェルで確認します。 |
| `winpe/startnet.cmd` | WinPE の起動時に、EHCI と USB デバイスの状態を診断して、画面とファイルに出します。 |
| `bl33/SHA256SUMS.txt` | 照合用です(基準の `bl33.bin` も入れています)。 |
| `contingency/` | **手順 A が `00` だった場合だけ**使う、補助ドライバ(未ビルド)と、その `startnet_fix.cmd`。 |

## 事前準備
1. SD カードを丸ごとバックアップします。元の `switchroot\winx\bl33.bin` は、別名で残してください(基準として使います)。
2. SD カードのルートに、空のファイル **`WINPE_LOG_HERE.txt`** を作ります(WinPE が診断の結果を、この SD に書き戻すための目印です。SD が読めない場合は、画面を撮影してください)。
3. WinPE は **Windows 10 ARM64 か、古い Windows 11(22000 / 22621 / 22631)の ADK** で作ります。24H2 以降は、Cortex-A57(ARMv8.0)では起動しません。

## 手順 A: UEFI で、リセット後のホストモードを確認する(約10分)
1. `bl33_wintest_acpiusb.bin` を `switchroot\winx\bl33.bin` として置き、起動して UEFI シェルに入ります。
2. 先に、普段の確認(`map`、`devices` など)を済ませます。**このスクリプトは、ファームウェアの USB を止めるので、最後に実行してください。**
3. `fs0:` に `ehci_reset_probe.nsh` を置いて、実行します。
4. 画面を撮影してください。見るのは **AFTER の USBMODE** の最初のバイトです。

| AFTER の USBMODE(最初のバイト) | 意味 | 次の一手 |
|---|---|---|
| `03` | リセットしてもホストモードが残る | 標準の usbehci で動く可能性があります。手順 B へ。 |
| `00` | リセットでホストモードが消える | **標準の usbehci では、ホストになりません。** 手順 B で確認はできますが、専用の対応(UCX ドライバか、リセット後に USBMODE を書き直すフィルタ)が必要です。 |
| その他 | 想定外 | 画面を送ってください。 |

## 手順 C(補助): 手順 A が `00` だった場合
`contingency\` に、USBMODE をホストに書き戻す補助ドライバ(`TegraEhciFix.c`)と、その使い方(`BUILD_AND_USE.md`)を入れてあります。**未ビルドで、WDK が要ります。** 手順 B の前に、イメージへ入れてください。

## どちらの案を先に試すか
Microsoft 自身の i.MX(ChipIdea)の ACPI が、**B 案と同じ構成**で、リセット後の `USBMODE` を `_UBF` で書き戻しています(GitHub の `ms-iot/imx-edk2-platforms`)。ただし、Tegra はレジスタの位置が違い(`USBMODE` が +0x1F8、標準の ChipIdea は +0x1A8)、標準の UrsChipidea がそれに合うかは分かりません。そのため、**A 案、B 案の順に1回ずつ**試して、レポートを比べてください。

| B 案で見ること | 意味 |
|---|---|
| `ACPI\PNP0C90\0` が無い | B 案の bl33 を使っていません。 |
| `PNP0C90` がデバイスとしてあるが、ドライバが無い | 標準の `urschipidea.inf` に `PNP0C90` が含まれていません(レポートの `findstr` の結果で分かります)。独自の INF が必要です。 |
| `Status: Started`、`USB` クラスに EHCI のホストが出る | 成功です。キーボードが出るか見てください。 |
| `Problem: 0x0A` / `0x2B` など | URS か ChipIdea のクライアントが、Tegra のレジスタの位置に合わず失敗した可能性があります。 |

## 手順 B: WinPE で確認する
1. WinPE のイメージを作ります。
   ```
   copype arm64 C:\WinPE_arm64
   Dism /Mount-Image /ImageFile:C:\WinPE_arm64\media\sources\boot.wim /Index:1 /MountDir:C:\WinPE_arm64\mount
   copy winpe\startnet.cmd C:\WinPE_arm64\mount\Windows\System32\startnet.cmd
   Dism /Unmount-Image /MountDir:C:\WinPE_arm64\mount /Commit
   MakeWinPEMedia /UFD C:\WinPE_arm64 <SDのドライブ>:
   ```
   (「カスタムデスクトップ版」が `startnet.cmd` を使わない場合は、自分の起動処理の先頭で `startnet.cmd` の中身を実行してください。)
2. **基準の確認:** 先に、元の `bl33.bin`(ACPI の USB なし)で WinPE を起動して、`NVDA0214` が**無い**ことを確認します。
3. `bl33_wintest_acpiusb.bin` に替えて、同じ WinPE を起動します。
4. 画面(key lines)を撮影してください。レポート全文は、`X:\winpe_usb_report.txt` と、`WINPE_LOG_HERE.txt` のあるボリュームの `winpe_usb_report.txt` に出ます。

## 結果の読み方
| 画面の様子 | 意味 |
|---|---|
| `NVDA0214` が無い | ACPI のテーブルが読まれていません(bl33 の取り違えか、`SWITCH_USB` なしのビルドです)。 |
| `Status: Started`、問題なし | usbehci がデバイスを開始しました。接続中の USB に、キーボードが出るか見てください。 |
| `Problem: 0x0A`(CM_PROB_FAILED_START) | usbehci が開始に失敗しました(ホストモードのリセット問題の可能性があります)。 |
| `Problem: 0x2B`(CM_PROB_FAILED_ADD)、`0x1C` | 割り込みやリソースの記述の問題の可能性があります。 |
| デバイスはあるがドライバが無い | WinPE のイメージに usbehci が入っていません(ADK のバージョンを確認)。 |
| `setupapi.dev.log` に `usbehci` が出ない | PNP0D20 に一致するドライバが選ばれていません。 |

## 注意
- **eMMC は、既定で Windows に見せません**(`-D SWITCH_ACPI_EMMC=1` を付けないでください)。
- 電源は、バッテリーが十分ある状態で。充電しながらの接続は、前の制限(抜き差しで USB が使えなくなる)に当たる可能性があります。
- うまくいかないときは、元の `bl33.bin` に戻すだけで、元の状態です。
- このキットは、実機と Windows では**まだ一度も試していません**。
