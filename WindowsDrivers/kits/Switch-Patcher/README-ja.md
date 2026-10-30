# SwitchPatcher

PC で動く道具です。Switch の SD を PC に挿して、SD の Windows に次のものを入れます。

- Switch のドライバー
- テストモードとテスト証明書
- コントロールセンター

Windows のイメージ（ARM64）を、SD に入れ直すこともできます。設計は `docs/Design-SwitchPatcher-ja.md`。

## 使い方

1. **SD のバックアップを取る。** 「Windows を入れる」では、SD の Windows の区画が消えます。
2. **起動する。** `SwitchPatcher.exe` を起動する。管理者の確認が出る。
3. **SD を選ぶ。** 「SD カード」と「Windows の区画」を選ぶ。SD は、Switch の SD のように、GPT で ESP と NTFS の区画があるものに限る。
4. **することを選ぶ。**
   - **入っている Windows にパッチを当てる:** 今の Windows はそのまま。
   - **Windows のイメージを入れる:** ISO、WIM、ESD と、ARM64 のエディションを選ぶ。Windows の区画をフォーマットして入れ直す。
   - **新しい SD に Windows を入れる（0.2.0 以降）:** SD 全体を消して、GPT・ESP（FAT32、260 MB）・MSR（16 MB、0.3.0 以降）・Windows の区画（NTFS、残り全部）を作ってから、イメージを入れる。
     - 消せるのは、カードリーダーの SD（SD、USB、MMC）だけ。
     - 消す前に、ダウンロードしたものを全部確かめる。
     - SD の大きさを入力しないと進まない。
5. **入れるものを選んで、「開始」を押す。**
   - 確認の画面が出る。
   - イメージを入れるときは、区画の大きさを入力する画面も出る。
6. **SD を外して、Switch で起動する。** 終わったら、SD を「安全に取り外す」から外す。

## 入れるもの

- **ドライバー:** GitHub の最新のリリースから取ってくる。入れる前に、次を確かめる。
  - ZIP の SHA-256
  - 各 .sys
  - 署名
- **テストモード:**
  - 証明書を、起動していない Windows の「信頼されたルート証明機関」と「信頼された発行元」に入れる。
  - ESP の BCD で `testsigning on` にする。
- **コントロールセンター:**
  - `C:\SwitchWOA\SwitchCenter\` に置く。
  - ログインしたときに起動する（`HKLM\...\Run`）。
- **SD からの起動（0.3.0 以降、リリースの `Switch-Sd`）:** SD の Windows のビルドを見て決める。
  - **Windows 11（ビルド 22000 以上）:** ESP の `EFI\Boot\bootaa64.efi` に `SDLOADAA64-NOCID.EFI` を置き、SD のドライバー nvtsdhc を入れて、起動時に読み込む（Start=0）。nvtsdhc はテスト署名なので、テストモードも必ず入れる。
  - **Windows 10:** 「Windows を入れる」ときだけ `SDLOADAA64.EFI` を置く。パッチだけのときは、ESP のローダーを変えない。
  - テストモードを入れるときは、BCD の起動のデバッグ設定（bootlog、sos、bootstatuspolicy、nocrashautoreboot）を外す。
- **Windows を入れたときだけ:**
  - 初回起動で、Microsoft アカウントとネットワークを求めないようにする（unattend.xml、BypassNRO）。
  - アカウントは、初回起動の画面で自分で作る。

ESP の起動ファイル（BCD など）は、書き換える前に `ドキュメント\SwitchPatcher\backup-<日時>\` に保存します。

Windows のイメージとライセンスは、各自で用意してください。

## 試したこと

- **仮想ディスク（VHDX、Switch の SD と同じ GPT・ESP・NTFS の形）での試験（2026-10-27）:** ARM64 の WinPE（10.0.26100）を相手に、「Windows を入れる」と「パッチ」を最後まで通した。
  - 展開と bcdboot（ESP のバックアップを含む）
  - 証明書（ROOT、TrustedPublisher）、自動起動、BypassNRO
  - 8つのドライバーの登録（`dism /Get-Drivers` で確かめた）
  - テストモード（`testsigning Yes`）
  - コントロールセンターの配置
- **0.2.0 の「新しい SD」（2026-10-27）:** 空の仮想ディスク（RAW）から試した。GPT・MSR・ESP・NTFS ができて、そのあとの展開とパッチも全部通った。
- **0.2.1:** 本物の SD（32 GB）で「新しい SD」を試したところ、消したあとの SD が MBR で初期化されたまま残り、GPT にできずに止まった。0.2.1 では、消したあとの形を見て、RAW なら初期化し、MBR なら GPT に変える。
- **0.2.2:** 0.2.1 で SD の初期化と Windows 11 22621 ARM64 の展開までは通ったが、PC（Windows 11 26100、x64）の bcdboot が「ブート ファイルをコピーしようとして、エラーが発生しました」（exit 193）で失敗した。0.2.2 では、bcdboot が失敗したら、`Windows\Boot\EFI` を ESP にコピーし、bcdedit で BCD を作り直す。
- **0.2.3:** 0.2.2 で作った Windows 11 22621 の SD は、Switch でロゴとくるくるまで進んだが、`INACCESSIBLE_BOOT_DEVICE` で止まった。
  - 理由: Switch の UEFI は、SD を「ベンダー独自の装置」として Windows に渡す。そのため Windows の起動ローダーは SD からの起動と分からず、SD のときだけ読み込むドライバー（sdbus、sdstor）を読み込まない。動いている Win10 の SD では、この2つが Start=0 になっていた。
  - 0.2.3 では、毎回のパッチで、オフラインの SYSTEM レジストリの sdbus と sdstor を Start=0 にする。
  - あとで分かった本当の理由: 新しい ESP の `bootaa64.efi` が `bootmgfw.efi` のままで、SD ホストの SSDT を足すローダーが無かった（UEFI の DSDT には SD ホストが無い）。
- **0.2.4:** 自分で作った BCD に `Description\System = 1` を足す。無いと、初回起動の設定（specialize）が「File is not system store」で失敗する。
- **0.3.0（2026-10-29）:** Windows 11 22621 は標準の sdbus では SD から起動できない（起動ドライブを読めないか、止まる）。手で試して動いた組み合わせ（NOCID のローダー、nvtsdhc 0.3.2、テストモード）を、パッチに入れた。新しい SD に MSR を作り、空のカードリーダー（0 GB）は一覧に出さない。
- **本物の SD と Switch:** 0.2.4 までで作った Win11 の SD に、手作業で 0.3.0 と同じ変更をしたものは、Switch でデスクトップまで動いた（SD の読み込み 18 MB/s）。0.3.0 そのものは、まだ試していない。
