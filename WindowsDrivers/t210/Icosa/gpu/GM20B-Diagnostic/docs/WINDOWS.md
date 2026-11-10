# winx-gm20b — Windows 11 ARM64 用 GM20B ドライバ手順書

対象: `gm20b.sys`(WDM / ARM64)・`gm20bctl.exe`・配布フォルダの `*.cmd`。
Nintendo Switch(Tegra X1)の GPU(GM20B)を、Windows から段階的に立ち上げるための**実験用ドライバ**です。

## 0. 先に読む(正直な現状)

- **このドライバは一度も Windows 上でロードされていません。** PC(Linux)上の検査だけが済んでいます。
- 実装済みのステージは **S0(MAP)・S1(POWER)・S2(ID: BOOT0 読み出し)** まで。S3〜S8 は未実装で、`MaxStage` を 3 以上にすると
  `NOTIMPL` を返し、S1 で入れた電源状態を戻して(GPU を再びリセット保持、こちらが有効にしたクロックだけ停止)終了します。
- 値は全て「理論値」(NVIDIA nvgpu r32.7.6 と nouveau の一次資料)で、実機で確定したものはありません。
  確度ラベル A/B/C は `DESIGN.md` §3、レジスタごとの出典は `provenance/` にあります。
- **「S1 が成功した」は「GPU に電源が入っている」ではありません**(§8 の 1)。S1 が見るのは SoC 内のビット(クロックゲート・
  リセット・クランプ)だけで、GPU 用の電源レール(PMIC)は見えません。

| PC 上で確認済み(`make check`) | 実機でしか分からないこと |
|---|---|
| コア 40 テスト(模擬 GM20B 上で S0〜S2 の順序、STEP の永続化、失敗時の巻き戻し) | Windows がこの .sys をロードするか(署名、PE の細部、HVCI) |
| 変異テスト 35 個(わざとバグを入れてテストが落ちるか: 全部検出) | 本物の ntoskrnl が、テスト用の偽物と同じ動きをするか |
| PE 構造(ARM64・WDM・チェックサム・インポートは ntoskrnl の 21 関数だけ・W+X なし・FP/SIMD 命令なし) | `ZwFlushBuffersFile` / `ZwFlushKey` が SD カードのメディアまで届くか |
| 構造体レイアウト 83 項目を mingw-w64 の DDK ヘッダと照合 | 物理アドレス 5 か所が `MmMapIoSpaceEx` で張れるか |
| 偽の ntoskrnl 上でシェル全体を動かすテスト(18 シナリオ、約 9 万チェック): NT API の引数が仕様どおりか、各記録がハードウェア・アクセスの**前に**永続化されるか、IOCTL、レジストリ、失敗経路、アンロード | CAR/PMC/BAR0 が理論どおり動くか(§8) |
| ジャーナルのデコード(擬似ハングの file と registry 値から原因行を特定できる) | |

## 1. ビルド

```
cd winx-gm20b
make check            # 全検査(1〜2 分)
make -C win dist      # build/dist/gm20b-win/ を作る(検査込み)
```

必要な物: clang 18 以上、lld-link、llvm-dlltool、python3(WDK も Windows SDK も不要)。
`make -C win` だけなら `build/win/gm20b.sys` と `gm20bctl.exe` ができます。

配布フォルダ `gm20b-win/` の中身:

| ファイル | 内容 |
|---|---|
| `gm20b.sys` | ドライバ(**未署名**) |
| `gm20bctl.exe` | 操作ツール(ユーザーモード) |
| `install.cmd` / `uninstall.cmd` | サービスの登録(デマンド起動)/ 削除 |
| `set-stage.cmd` / `start.cmd` / `stop.cmd` | ステージ設定 / 開始 / 停止 |
| `collect-logs.cmd` | ジャーナルとレジストリを `gm20b-logs\<日時>\` に集める |
| `offline-install.cmd` | WinPE などから、オフラインの Windows に登録する |
| `journal_decode.py` / `jcodes.tsv` | PC 側のジャーナル解読ツールとコード表 |
| `examples/` | PC 上の模擬環境で本物のシェルコードが作ったジャーナルと、そのデコード結果(時刻は模擬) |
| `provenance/` | S1/S2 のレジスタ表(出典・確度・実機での確かめ方) |
| `DESIGN.md` / `NOTICE.md` | 設計(確度ラベル A/B/C の定義など)/ ライセンスと第三者の資料 |
| `gm20b.map` | リンクマップ(アドレスの確認用) |
| `SHA256SUMS` | 署名前のハッシュ |

## 2. 署名(あなたのテスト証明書で)

`gm20b.sys` は未署名です。**このリポジトリは鍵も証明書も生成せず、埋め込みもしません。** 署名はあなたの既存の
テスト証明書(オフラインで登録済みのもの)で行ってください。Windows 側の前提は、テスト署名モード、証明書が信頼されていること、
メモリ整合性(HVCI)がオフであること。

```
signtool sign /fd sha256 /f あなたの証明書.pfx /p パスワード gm20b.sys
```

Linux 上で署名するなら `osslsigncode sign -certs cert.pem -key key.pem -h sha256 -in gm20b.sys -out gm20b-signed.sys`
(出力を `gm20b.sys` に改名)。どちらの署名コマンドもこちらでは試していません。
署名すると `SHA256SUMS` の `gm20b.sys` の行は一致しなくなります(正常)。

## 3. 土曜日の手順

管理者のコマンドプロンプトで、配布フォルダ(例: `C:\winx\gm20b-win`)から実行します。
Windows に入らずに登録したいときは、WinPE から `offline-install.cmd D:`(D: は Windows のドライブ)。
どちらもサービスは**デマンド起動**(Start=3)です。起動時に自動で読み込まれることは決してありません。

方針: **安全なものから順に 1 段ずつ**。各段の後に `collect-logs.cmd` を実行して記録を残します。
ハングしたら、その週はそこで止めて(§4)ログを持ち帰ります。

### A. ロードだけ(ハードウェアには触れない)

```
install.cmd
start.cmd
gm20bctl status
stop.cmd
```

期待: `sc start` が成功し、`last init status 0 (OK)`、`journal file ... bytes written` が増え、`J_WIN_MAP` が 5 本(a=0〜4)で
`b` が 0 でない。`C:\Windows\gm20b-journal.bin` ができ、レジストリに `LastEntry` が付く。
見えるもの: ロードされるか、レジスタ窓が張れるか、ジャーナルが書けるか。

### B. 読むだけ(S1 の DIAG_ONLY)

```
set-stage.cmd 1 10
start.cmd
```

`Flags` の `10`(16 進)は DIAG_ONLY です。CAR/PMC のレジスタを 5 本**読むだけ**で、何も書きません(読む前に 1 本ごとに
`J_S1_PROBE_RD` が永続化されます)。`J_S1_PRE_*` に現在の状態が出ます:

| 記録 | 見方 |
|---|---|
| `J_S1_PRE_CLK`(CAR 0x280) | bit24 = GPU クロックゲート(0 = 停止) |
| `J_S1_PRE_RST`(CAR 0x28c) | bit24 = GPU がリセット中(1 = リセット) |
| `J_S1_PRE_CLAMP`(PMC 0x2d4) | bit0 = クランプ/ゲート中(1 = 通電していない側) |
| `J_S1_PRE_PLLP_BASE` / `_OUTC`、`J_S1_PWR_CLK` | PMU 用クロックの参考値(確度 C、後のステージ用) |

見えるもの: CAR/PMC に Windows から触れるか、現在の状態。

### C. 電源投入(S1、ここから書き込みあり)

```
stop.cmd
set-stage.cmd 1 0
start.cmd
```

書き込みは 4 回だけ: CAR のクロック有効化(SET エイリアス: GPU と PLLG_REF)→ リセット保持(SET)→ PMC クランプ解除 → リセット解除(CLR)。
どれも直前に `STEP` 記録を永続化します。最後に 3 つのビットを読み戻して確認します。
成功: `J_STAGE_OK a=1`、`J_S1_POST_CLK` の bit24=1、`J_S1_POST_RST` の bit24=0、`J_S1_POST_CLAMP` の bit0=0。
失敗: `J_S1_VERIFY_FAIL`(a = 1 クロック停止のまま / 2 リセットのまま / 4 クランプのまま)。a=4 なら PMC への書き込みが
無視された疑い(PMC が特権側のみ)。

### D. BOOT0(S2、ここが最初の BAR0 アクセス = 最も危険)

```
stop.cmd
set-stage.cmd 2 0
start.cmd
```

BAR0 の前に CAR/PMC を読み直して「通電」を確認し、だめなら **BAR0 に触らず** `E_STATE` で止まります
(`Flags` に `10000` を付けるとこの確認を飛ばします。確認自体を疑うとき専用)。確認が通ると
`J_S2_BOOT0_INTENT`(STEP)を永続化してから BOOT0 を読みます。
成功: `J_S2_BOOT0 a=12b000a1`(GM20B: チップセット 0x12b)、`J_S2_DECODE a=12b b=12b`、`J_STAGE_OK a=2`。
S2 は PMC_ENABLE を読むだけで何も有効にしません。割り込みは使わない(ポーリング運用)ので、MC の割り込みレジスタ 4 本に
「無効」を書きます(BOOT0 が GM20B と確認できた後だけ。DIAG_ONLY では書きません)。

### 1 回の起動で進めたいとき

ドライバを読み込んだまま `gm20bctl advance 1`、`gm20bctl advance 2` と進められます。`gm20bctl fini` で S1 の電源状態を戻して
最初に戻れます。

### ハングしたら

1. 電源を長押しで切り、Windows を起動し直す(入れないときはヒント: §4 の「Windows が起動しないとき」)。
2. `collect-logs.cmd` を実行し、できた `gm20b-logs\<日時>\` を持ち帰る(`journal.bin`・`registry.txt`・`journal.txt`・`status.txt`)。
3. その週はそこで止める。次に進むかどうかはログの最終行で決める。

## 4. 結果の読み方

### status

`gm20bctl status` の見どころ: `max stage`(設定値)、`stages done`(完了数: S0 だけなら 1、S2 まで通れば 3)、
`last init status`、失敗したステージ、`journal file ... write errors / dropped (cap)`、`registry crumb ... errors`、
`refused MMIO accesses`(HAL が拒否した不正アクセス数)。

### ジャーナル

| 場所 | 中身 |
|---|---|
| `C:\Windows\gm20b-journal.bin` | 追記(ライトスルー + 書くたびにフラッシュ)。上限は 1 回のロードにつき 2 MiB |
| `HKLM\SYSTEM\CurrentControlSet\Services\gm20b\LastEntry` | **最新の 1 件**(24 バイトの REG_BINARY、書くたびに `ZwFlushKey`)。2 MiB の上限を超えた後も追従する |

読み方:

```
gm20bctl decode                                   (Windows 上。ドライバ不要。既定は C:\Windows\gm20b-journal.bin)
python journal_decode.py journal.bin --explain    (PC 上。最後の記録の意味も出る)
reg query HKLM\SYSTEM\CurrentControlSet\Services\gm20b /v LastEntry
python journal_decode.py --hex <上で出た 16 進>   (レジストリの 1 件を解読)
```

ファイル形式: 32 バイトのヘッダ(`G20J`)+ 24 バイトのレコード(seq, code, a, b, 時刻 ns)。シェルの記録は seq が
`0xF0000000` 以上、コアの記録は 0 から。`J_WIN_LOAD` でドライバのロードが区切られる。

**`STEP` 付きの記録は「これからアクセスします」を、アクセスの前に永続化したもの**です。ハングした実行の最後の記録が STEP なら、
その直後のアクセス(その STEP が予告したもの)でバスが止まったということです。ファイルとレジストリの両方に残るので、
新しい方(seq が大きい方)を使ってください。SD カードのキャッシュ次第で、最後の数件がファイルから欠けることがあります。
`examples/hang-at-boot0.txt` は、BOOT0 の読み出しでハングした場合にファイルとレジストリに残る内容(PC 上の模擬)です。

### 最後の記録が…だったら

| 最後の記録 | 意味 | 次の手 |
|---|---|---|
| ファイルが無い / `J_WIN_LOAD` が増えない | `DriverEntry` が走っていない | 署名、テストモード、HVCI、`sc qc gm20b`、イベントビューアの CodeIntegrity |
| `J_WIN_MAP` の `b=0` | その窓の `MmMapIoSpaceEx` が失敗(a: 0=BAR0 1=BAR1 2=CAR 3=PMC 4=MC)。ハードウェアには一切触れていない | 別の窓か、物理アドレスの占有を調べる |
| `J_S1_PROBE_RD` | その a=(空間<<28 \| オフセット)の読み出しでハング。空間は 2=CAR、3=PMC | CAR/PMC がこの環境で読めない。`gm20bctl rawread` で 1 本ずつ確かめる |
| `J_S1_CLK_ENABLE` / `J_S1_RST_ASSERT` / `J_S1_CLAMP_REMOVE` / `J_S1_RST_DEASSERT` | その書き込みでハング | 同上(書き込みが特権側のみ) |
| `J_S1_VERIFY_FAIL` | ハングではなく検証失敗。a のマスクを見る(§3 C) | 書き込みが効いていない |
| `J_S2_GATED` | S2 の確認で「通電していない」と判断し、BAR0 に触らず停止(a のビット: 1 クロック / 2 リセット / 4 クランプ) | S1 の `POST_*` を見る |
| `J_S2_BOOT0_INTENT` | **最初の BAR0 アクセス(BOOT0)でバスハング** | 電源理論が外れている(§8 の 1)。S1 の `POST_*`・`PRE_*` が手がかり |
| `J_S2_BAD_BOOT0` | ハングせずエラー応答(a=0xffffffff や 0xbadfXXXX = バスエラーのパターン、a=0 = 何も応答していない) | GPU が通電していない典型 |
| `J_S2_WRONG_CHIP` | 読めたが GM20B ではない(a=読んだチップセット) | レジスタ窓の物理アドレスを疑う |
| `J_WIN_INIT_DONE a=0` | 指定した段階まで成功 | 次の段へ |

Windows が起動しないとき: SD を PC に出し(hekate の UMS)、`\Windows\gm20b-journal.bin` を PC の `journal_decode.py` で読む。
レジストリは `reg load HKLM\SW X:\Windows\System32\config\SYSTEM` のあと
`reg query HKLM\SW\ControlSet001\Services\gm20b /v LastEntry`(終わったら `reg unload HKLM\SW`)。

## 5. レジストリ設定

`HKLM\SYSTEM\CurrentControlSet\Services\gm20b\Parameters`(全て REG_DWORD。型が違う・無い値は既定値になり、実際に使った値と
由来が `J_WIN_REG_*` に記録されます: `b=1` レジストリ由来 / `b=0` 既定)。変更は**次のロード(`stop.cmd` → `start.cmd`)から**有効。

| 値 | 既定 | 意味 |
|---|---|---|
| `MaxStage` | 0 | 実行する最後のステージ(0〜8)。0 = レジスタ窓を張って HAL を確認するだけ(ハードウェアに触れない) |
| `Flags` | 0 | ビット: `0x1` TRACE_REGS(全 MMIO を記録。フラッシュは都度ではないので、ハング直前の記録の保証にはならない。保証があるのは STEP だけ)、`0x4` ALLOW_RAIL(下記)、`0x10` DIAG_ONLY(読むだけ)、`0x10000` FORCE_BAR0(通電確認を無視)。`0x2` USE_PLL と `0x8` SELFTEST は S3 以降用 |
| `GpcKhz` | 0 | S3 の目標周波数(S3 は未実装) |
| `RailUv` | 0 | S1 の電圧要求(µV)。ALLOW_RAIL とフックの両方が必要。**Windows 版はフックを持たない**ので無効 |
| `PollScalePct` | 0(= 100) | 全ての待ち時間の上限を何 % にするか |
| `AllowRawMmio` | 0 | 1 で生レジスタの読み書き IOCTL(`rawread` / `rawwrite`)を有効にする |

`set-stage.cmd N [flags-hex]` は `MaxStage` と(指定すれば)`Flags` を書きます。`Flags` を消すには `set-stage.cmd 1 0`。
`AllowRawMmio` は意図的に `set-stage.cmd` では設定できません:

```
reg add HKLM\SYSTEM\CurrentControlSet\Services\gm20b\Parameters /v AllowRawMmio /t REG_DWORD /d 1 /f
```

## 6. gm20bctl と生レジスタ IOCTL

```
gm20bctl status
gm20bctl journal [from_seq]               ドライバ内のリング(直近 512 件)
gm20bctl advance <0..8>                   次のステージまで進める(ドライバは読み込んだまま)
gm20bctl fini                             電源状態を戻し、次の advance は最初から
gm20bctl rawread  <bar0|bar1|car|pmc|mc> <offset>
gm20bctl rawwrite <bar0|bar1|car|pmc|mc> <offset> <value>
gm20bctl decode [file]                    ジャーナルファイルを解読(既定 C:\Windows\gm20b-journal.bin)
```

IOCTL は 1 つずつ直列に処理され(同時に来たら `STATUS_DEVICE_BUSY`)、制御デバイスは排他オープンです。`rawread` / `rawwrite` の規則:

| 空間 | 読み | 書き |
|---|---|---|
| `bar0` / `bar1` | 可。ただし CAR/PMC が「通電していない」と言う間は拒否(`FORCE_BAR0` で解除) | 同左 |
| `car` | 可。ただし SET/CLR エイリアス(0x284 / 0x288 / 0x290 / 0x294)は読めない(拒否) | **SET/CLR エイリアスの 4 本だけ**(共有レジスタの直接書き込みは拒否) |
| `pmc` | 可 | 0x2d4(GPU_RG_CNTRL)だけ |
| `mc` | 可 | 拒否(MC は読み取り専用) |

どのアクセスも、直前に `J_WIN_RAWRD` / `J_WIN_RAWWR`(STEP)を永続化します。BAR0/BAR1 の通電確認が CAR/PMC を読むときも
`J_WIN_RAW_GUARD`(STEP)を先に永続化します。拒否されたものは `J_WIN_RAW_DENIED`(b: 1 設定でオフ / 2 規則 / 3 未通電 / 4 不正な要求)。
アライメント違反・範囲外は HAL が実行前に弾き、`J_WIN_BAD_ACCESS` に記録します(最初の 16 件まで)。

## 7. 安全設計(触るもの・触らないもの)

- **PMIC・電圧には既定で触れません。** 電圧の変更は `ALLOW_RAIL` が立ち、フックが設定され、値が 800000〜1050000 µV のときだけ可能です。
  Windows 版はフックを持たないので、実際には起こりえません。
- **CAR(0x60006000)は ACPI の `SWOA0701`「CPUC」と共有**です。書くのは SET/CLR エイリアスだけ(読み-修正-書きはしない)。
  PLLP_OUT5(`pwr` クロック)は読むだけです。
- **MC / SMMU は読み取り専用**です。
- MMIO は全て 32 ビット・整列(HAL が整列違反と範囲外を拒否し、`-mstrict-align` でコンパイラの勝手な幅広アクセスも出さない)。
- 待ちは全て上限付き(`PollScalePct` で倍率変更)。割り込みは使わず、全て `PASSIVE_LEVEL` のポーリング。
- サービスは**デマンド起動のみ**。ブート起動にはしません。
- `DriverEntry` はブリングアップが失敗しても `STATUS_SUCCESS` を返し、ドライバは読み込まれたままになります(状態を調べるため)。
  既定の `MaxStage=0` ではハードウェアに触れません。
- ハングしても最後の意図が分かるよう、危険な操作の前に必ず記録を永続化します(`STEP`)。

## 8. 未検証の前提とリスク(危険度順)

| # | 前提 | 外れたときに起きること | 見分け方 |
|---|---|---|---|
| 1 | **GPU の電源レール(VDD_GPU、MAX77621)が既に入っている。** EDK2 経由の起動では誰も上げていない可能性が高い | S1 は成功して見える(見ているのは SoC 内のビットだけ)が、S2 の BOOT0 が 0xffffffff / 0xbadfXXXX、またはハング | `J_S2_BAD_BOOT0` または最終行が `J_S2_BOOT0_INTENT`。レールを上げるには I2C5(0x7000d000)の PMIC 制御が別途必要(**未実装**。既定で触れない設計) |
| 2 | Windows がこの .sys を読み込む | `sc start` がエラー(577 署名/ハッシュ、193 PE 形式、1275 ブロック)。`DriverEntry` が走らないのでジャーナルは作られない | ファイルが無い、`J_WIN_LOAD` が増えない |
| 3 | CAR/PMC が Windows の動く権限から読み書きできる(CAR は CPUC が使うので読み書き可の公算が大きい。PMC 0x2d4 は不明) | S1 のプローブ読み出しでハング、または書き込みが無視される | `J_S1_PROBE_RD` が最終行 / `J_S1_VERIFY_FAIL` |
| 4 | BAR0(0x57000000)の最初の読み出しが成功する | バスハング | `J_S2_BOOT0_INTENT` が最終行 |
| 5 | 5 つの物理アドレスが `MmMapIoSpaceEx`(PAGE_NOCACHE)で張れる | その窓が使えない。ハードウェアには一切触れず `init_status` が NOMEM | `J_WIN_MAP` の `b=0` |
| 6 | ジャーナルがメディアまで届く(`FILE_WRITE_THROUGH` + `ZwFlushBuffersFile` + `ZwFlushKey`) | ハング時に最後の数件が欠ける | ファイルの最終行がレジストリの `LastEntry` より古い |
| 7 | SMMU がバイパス、GPU は CPU 物理アドレスを直接使える(S5 以降の前提) | S5 以降で DMA 失敗 | ステージ 4 が MC の SMMU ビットを読んで記録する予定(未実装) |
| 8 | CPUC との CAR 共有で競合しない | SET/CLR エイリアスのみ書くので起きにくい | — |

整列違反は起きる前提で作ってあります(上の §7)。それでも出たら `J_WIN_BAD_ACCESS` が先に出るはずで、ハードウェアには届きません。

## 9. 未実装・これから

- **S3〜S8(GPCPLL、priv ring・fuse・FB・SMMU 診断、GMMU/BAR1、PFIFO、ACR/FECS/GPCCS、GR)は未実装です。**
- ファームウェアの読み込み(`fw_get`)は未実装(S7 以降で必要。NVIDIA のファームウェアは同梱しません)。
- PnP / ACPI のデバイスノード化は未着手(`bl33` の DSDT に GPU ノードは無く、このドライバは固定物理アドレスのレガシー WDM として動く)。
- 表示(DC/DSI)は別の話題で、このドライバは触れません。
