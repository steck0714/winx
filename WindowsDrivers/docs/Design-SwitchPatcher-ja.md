# SwitchPatcher（PC で SD の Windows にドライバーを入れる道具）：設計

作成: 2026-10-27（Claude）。配布の計画（「組み立てる道具」）の最初の版。

## 目的

PC に Switch の SD を挿して、次の2つのどちらかをする。

| 作業 | 内容 |
|---|---|
| **パッチを当てる** | SD に入っている Windows に、ドライバー、テストモード、証明書、コントロールセンターを入れる |
| **Windows を入れる** | 利用者が用意したイメージ（ISO、WIM、ESD）の ARM64 版を、SD の Windows の区画に入れ直す。そのあと同じパッチを当てる。Windows 11 を試すためにも使う |

Windows そのものは配らない。イメージとライセンスは、利用者が用意する（`Design-Distribution-ja.md`）。

## 前提（今の SD の作り）

- GPT の SD に、ESP（FAT32）と Windows の区画（NTFS）がある。
- Switch の UEFI は、ESP の `EFI\Boot\bootaa64.efi` から Windows のブートマネージャーを起動する。
- テストモードは、ESP の `EFI\Microsoft\Boot\BCD` の `{default}` に `testsigning on` を書いて有効にする。Switch の上からは書けないので、PC から書く。

- **今ある SD:** 区画は作らない。ESP もフォーマットしない。Windows を入れるときは、Windows の区画だけを NTFS でフォーマットし直す。
- **新しい SD（0.2.0）:** SD 全体を消して、GPT・ESP・NTFS を作る。
  - ESP は、最初はデータの区画として作り、FAT32 でフォーマットしてから種類を ESP に変える。Windows は、ESP の種類の区画を Format-Volume で開かせないため。
  - 消す前に、ダウンロードと確かめを全部終える。

## 安全のための決まり

- PC が起動しているディスク（システム、ブート）は、一覧に出さない。
- 書き込む前に、何をするかの一覧を見せて確認する。フォーマットは、ドライブ文字を入力してもらってから行う。
- ESP の `EFI\Microsoft\Boot` と `EFI\Boot\bootaa64.efi` は、書き換える前に PC にバックアップする（`ドキュメント\SwitchPatcher\backup-<日時>\`）。
- ドライバーとコントロールセンターは、OTA と同じ確かめ方をする（コードも共通）。
  1. HTTPS で、このプロジェクトのリリースから取る。
  2. ZIP の SHA-256 を manifest と照らし合わせる。
  3. 各 .sys の SHA-256 を manifest と照らし合わせる。
  4. 各 .cat の署名者を、テスト証明書の thumbprint と照らし合わせる。
- 作業用のフォルダーは、管理者だけが書ける場所に作る。

## パッチの中身

| 段階 | やり方 |
|---|---|
| 証明書 | 起動していない Windows の SOFTWARE ハイブを読み込み、`ROOT` と `TrustedPublisher` に証明書を書く。証明書の Blob は、PC の一時的な証明書ストアに一度入れて作る |
| ドライバー | `dism /Image:W:\ /Add-Driver` を manifest の順（GPIO、I2C、…）で行う |
| テストモード | ESP の BCD をバックアップしてから `bcdedit /store … /set {default} testsigning on` |
| コントロールセンター | `W:\SwitchWOA\SwitchCenter\` に展開する。普通のユーザーが書ける場所なので、自分自身の更新もできる。`HKLM\…\Run` に足して、ログインしたときに起動する |

- すでにドライバーが入っている Windows では、dism はドライバーを Windows に登録するだけで、動いているデバイスのドライバーは入れ替わらない。入れ替えは、コントロールセンターの OTA で行う。

## Windows を入れる中身

1. ISO なら PC でマウントして、`sources\install.wim`（または `.esd`）を使う。
2. エディションの一覧から、ARM64 のものだけを選べるようにする。
3. Windows の区画を NTFS でフォーマットし直す。
4. `dism /Apply-Image` で展開する。
5. ESP をバックアップしてから、`bcdboot W:\Windows /s S: /f UEFI` で起動の設定を作り直す。
6. 初回起動の設定を置く。
   - `Windows\Panther\unattend.xml`: Microsoft アカウントと Wi-Fi の画面を出さない。
   - オフラインの `BypassNRO`: ネットワーク無しで進める。Switch では、まだ Wi-Fi が使えないため。
   - ユーザーは、初回起動の画面で自分で作る。パスワードを道具に入れることはしない。前にログインできなくなった問題（オフラインで Winlogon を書き換えたこと）も避けられる。
7. 上のパッチを当てる。

## 作り

- **場所:** `app/SwitchPatcher/`
- **作り:** .NET 8、WinForms、win-x64、管理者として起動する（app.manifest）。
- **共通のコード:** リリースの読み込みと確かめは、コントロールセンターと共通（`app/Shared/ReleaseClient.cs`）。
- **配り方:** リリースに `SwitchPatcher-<版>-x64.zip` を足す。manifest にも載せる。

## まだやらないこと

- hekate の設定（`hekate_ipl.ini`、ペイロード）と、hekate の区画と並べること
- MBR の SD
- すでに入っているドライバーの入れ替え（コントロールセンターの OTA で行う）
