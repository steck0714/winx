# Windows ドライバー：GPIO・ボタン・I2C の設計と進め方（2026-10-24）

## 前提（調査結果）

### ボタン（L4T の `running-decompiled.dts` の `gpio-keys`。どれもアクティブ Low）

| ボタン | GPIO | 番号 | 備考 |
|---|---|---|---|
| 音量 − | **PX7** | 191 | |
| 音量 ＋ | **PX6** | 190 | |
| 電源 | PX5 | 189 | DT では `status = "disabled"`。PMC のウェイク 24 に対応している |

電源ボタンの本命は、PMIC の MAX77620 の ONOFF（I2C5、`ONOFFSTAT` の EN0）。Hekate の `btn.c` もこれを読んでいる。PX5 にも電源ボタンが反映されているかは、UEFI で読んで確かめる。

### GPIO コントローラー

| 項目 | 値 |
|---|---|
| MMIO | `0x6000D000`、長さ 0x1000 |
| 構成 | 8 バンク × 4 ポート × 8 ピン |
| レジスター | CNF 0x00、OE 0x10、OUT 0x20、IN 0x30、INT_STA 0x40、INT_ENB 0x50、INT_LVL 0x60、INT_CLR 0x70。マスク付き書き込みは +0x80 |
| 割り込み | GIC SPI 32、33、34、35、55、87、89、125（バンク 0〜7）。ACPI の GSI では 64、65、66、67、87、119、121、157 |

### I2C コントローラー（6 本）

| バス | アドレス | つながっている主な機器 |
|---|---|---|
| I2C1 | 0x7000C000 | BM92T、BQ24193、MAX17050、TMP451 など |
| I2C2 | 0x7000C400 | |
| I2C3 | 0x7000C500 | タッチ FTS（0x49） |
| I2C4 | 0x7000C700 | |
| I2C5 | 0x7000D000 | PMIC：MAX77620（0x3C）、MAX77621（0x1B/0x1C） |
| I2C6 | 0x7000D100 | |

### ビルド環境（この PC）

- **ある:** WDK のヘッダーとライブラリ（10.0.19041 / 26100）。km arm64、GpioClx（`msgpioclxstub.lib`、`gpioclx.h`）、SpbCx（`spbcxstubs.lib`、`spbcx.h`）、KMDF（`wdfldr.lib`、`wdfdriverentry.lib`）を含む。ほかに `signtool`、`MakeCert`、`Inf2Cat`。
- **ある（コンパイラー）:** Codex が SDHC ドライバーで使った zig 0.15.1（clang）。`D:\Codex\2026-09-06\new-chat-4\work\zig\...`
- **無い:** ドライバー（.sys）をリンクできるリンカー。Codex は Visual Studio の `link.exe` を使っていたが、今の VS（`C:\Program Files\Microsoft Visual Studio\18`）にはリンカーが見つからない。

## 進め方（段階）

| 段階 | 内容 | 成果物 | 確認方法 |
|---|---|---|---|
| 0 | **ビルド環境と署名** | (a) リンカーを入れる。LLVM（`lld-link`）か、Visual Studio Build Tools の ARM64 一式。(b) Switch の Windows を **テスト署名モード**（`bcdedit /set testsigning on`）にし、自己署名の証明書でドライバーに署名する | 何もしない空のドライバーが読み込まれる |
| 1 | **GPIO コントローラーのドライバー**（GpioClx、KMDF）と、DSDT への GPIO の追加（UEFI の新しい版） | `tegra210gpio.sys` と、DSDT の `GPI0` | デバイスマネージャーで正常に動作 |
| 2 | **音量ボタン**。DSDT に `PNP0C40`（Windows の標準のボタン配列）を追加し、GpioInt で PX6 と PX7 を割り当てる。Windows 標準のドライバーで動く想定 | DSDT のみ | 音量の表示（OSD）が出る |
| 2b | **電源ボタン（PX5 が使える場合）**。PX5 にも反映されていれば、同じ `PNP0C40` に追加する | DSDT のみ | 電源ボタンで画面オフやスリープのメニューが出る |
| 3 | **I2C コントローラーのドライバー**（SpbCx）と、DSDT への I2C1〜I2C6 の追加 | `tegra210i2c.sys` | 各バスが正常に動作 |
| 4 | **電源ボタン（PMIC 経由）**。PX5 が使えない場合に、MAX77620 の ONOFF の割り込みから、HID の電源ボタンにするドライバーを作る | 小さな KMDF ドライバー | 電源ボタン |
| 5 | **その後**。I2C の上に、バッテリー（MAX17050）、タッチ（FTS）、温度とファンなど | | |

## 注意

- 自作のドライバーは、Windows の起動を止める可能性がある。それぞれの段階で、SD を PC に挿して戻せるように、ドライバーを無効にする手順を用意する。
- DSDT の変更は UEFI の ROM を作り直す必要がある。R366 に積み重ねて、R367 以降とする。
