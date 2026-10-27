# TegraEhciFix(補助ドライバ)の使い方 ― 手順 A で `USBMODE = 00` だった場合だけ

**未ビルド・未検証です。** Windows のカーネルドライバなので、このソースは WDK でのビルドが必要です。構文だけ、簡易な宣言(スタブ)で確認しました。

## 何をするか
標準の usbehci が HCRESET した後、Tegra の EHCI は `USBMODE.CM = 0`(アイドル)になります。このドライバは、専用のスレッドで `USBMODE` を見張り、リセットが終わった直後に `CM = 3`(ホスト)を書き戻します。アドレスは Switch 用に固定です(USB1 = `0x7D000000`、`USBCMD` = +0x130、`USBMODE` = +0x1F8)。

## ビルド(Windows PC)
1. Visual Studio 2022 と、**ARM64 向けの WDK** を入れます。
2. 「Empty WDM Driver」のプロジェクトを作り、`TegraEhciFix.c` を追加します。
3. 構成は **Release / ARM64** です。
4. 出来上がった `TegraEhciFix.sys` に、テスト証明書で署名します(`signtool sign /a /fd sha256 TegraEhciFix.sys`)。

## WinPE への入れ方
1. イメージをマウントして、`TegraEhciFix.sys` を `Windows\System32\drivers\` にコピーします。
2. `startnet.cmd` を、`contingency\startnet_fix.cmd`(名前を `startnet.cmd` に変える)に差し替えます。
3. WinPE のメディアの BCD で、署名の検査を緩めます(`<メディア>\EFI\Microsoft\Boot\BCD`)。
   ```
   bcdedit /store <メディア>:\EFI\Microsoft\Boot\BCD /set {default} testsigning on
   ```
   (署名しない場合は、`nointegritychecks on`。)

## 動作
- 起動時に `sc create` と `sc start` で読み込み、`pnputil /restart-device` で EHCI を再起動します。usbehci が、もう一度リセットを行うのは、補助ドライバが動いた**後**です。
- 診断のレポートに `sc query TegraEhciFix` の結果が入ります。
- `DbgPrint` のログは、カーネルデバッガが無いと見えません。見るべきは、レポートの `NVDA0214` の `Status` です。

## 注意
- スレッドは、1つの CPU コアを使い切ります(テスト専用)。
- 効果があるかは、手順 A の結果と、WinPE のレポートで判断してください。**効かない場合は、別の原因です。**
