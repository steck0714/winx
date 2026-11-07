# Windows for Switch — Drivers

Windows 10 on ARM (ARM64) で動く、Nintendo Switch（Tegra210 / Erista）用のドライバーと、コントロールセンター（SwitchCenter）です。

UEFI と DSDT は [Windows-for-Switch](https://github.com/hayato41200/Windows-for-Switch) にあります。このリポジトリのドライバーは、そちらの UEFI（R367 以降）が DSDT に追加するデバイスを使います。

English summary: KMDF drivers (GPIO, I2C, battery + USB-C role, fan + backlight, touch screen,
power button, CPU clock, RTC) and a tray / control-center app for Windows 10 ARM64 on the
Nintendo Switch. Install from the release ZIPs; build with `release/Make-Release.ps1`.
Tested on Windows 10 IoT Enterprise LTSC 2021 (19044) ARM64; bring your own Windows image and license.

## ドライバー

| ドライバー | デバイス | 必要な UEFI | 内容 |
|---|---|---|---|
| `tegra210gpio` | ACPI\SWOA0101 | R367 | GPIO コントローラー（GpioClx） |
| `tegra210i2c` | ACPI\SWOA0201 | R370 | I2C コントローラー（SpbCx）。I2C1 / I2C3 / I2C5 |
| `switchbattery` | ACPI\SWOA0301 | R371（USB-C は R378） | バッテリー（battc）、USB-C の抜き差しに合わせた VBUS・充電の切り替え |
| `switchfan` | ACPI\SWOA0401 | R372（明るさは R376） | 温度によるファンの制御、画面の明るさ、画面オフでのバックライトの消灯 |
| `switchtouch` | ACPI\SWOA0501 | R373 | タッチパネル（VHF の HID マルチタッチ） |
| `switchpwrbtn` | ACPI\SWOA0601 | R374 | 電源ボタン（VHF の HID） |
| `switchcpu` | ACPI\SWOA0701 | R375 | CPU のクロックのプロファイル（614〜1766 MHz）、温度による制限 |
| `switchrtc` | ACPI\SWOA0801 | R377 | 時計（MAX77620 の RTC を読み、ずれを覚えて時刻を合わせる） |
| `swoadiag` | —（読み込んだときだけ） | — | USB（XUSB）のレジスタを読むだけの診断用 |

## 動作を確かめた OS

- Windows 10 IoT Enterprise LTSC 2021（ビルド 19044）ARM64

Windows のイメージとライセンスは、各自で用意してください。このリポジトリには、Windows は含まれていません。

## 入れ方

1. [Releases](https://github.com/hayato41200/Windows-for-Switch-Drivers/releases) から、最新の `SwitchDrivers-<版>.zip` と `SwitchCenter-<版>-arm64.zip` を取ってくる。
2. **テスト署名の準備（最初の1回だけ）:**
   - `Switch-Stage0` の手順で、テスト署名モードを有効にする。
   - 同梱の `SwitchWOA-Test.cer` を、Switch の「信頼されたルート証明機関」と「信頼された発行元」に入れる。
3. **ドライバーを入れる:** ZIP を展開して、各 `Switch-*` フォルダーのスクリプトで、次の順に入れる。手順は、各フォルダーの README にある。
   1. GPIO（Stage1）
   2. I2C（Stage3）
   3. バッテリー
   4. ファン
   5. タッチ
   6. 電源ボタン
   7. CPU
   8. RTC
4. **コントロールセンターを使う:** `SwitchCenter-<版>-arm64.zip` を展開して、`SwitchCenter.exe` を起動する。通知領域のアイコンから、次のことができる。
   - CPU のプロファイル
   - 画面の明るさ
   - 状態の確認
   - 電源の設定
   - ドライバーの一覧

## ビルド（PC）

必要なもの:
- LLVM（clang-cl、lld-link）
- WDK のヘッダーとライブラリ（10.0.19041 と 10.0.26100）
- .NET SDK（.NET 8 を対象にできるもの）
- 証明書ストアにある、テスト署名の証明書（「CN=SwitchWOA Test Driver Signing」）。秘密鍵はリポジトリに入っていない。

```powershell
.\release\Make-Release.ps1 -Version 2026.10.26
```

`out\<版>\` に、リリースに付ける3つのファイルができる。
- `SwitchDrivers-<版>.zip`
- `SwitchCenter-<版>-arm64.zip`
- `manifest.json`

GitHub でリリースを作って（タグは `drivers-<版>`）、この3つを付ける。

GitHub CLI（`gh auth login` 済み）があれば、`-Publish` を付けるとリリースの作成とアップロードまで自動で行う。すでに作ったファイルをあとから公開するときは、`-PublishOnly` を使う。

## OTA（コントロールセンターからの更新）

設計は `docs/Design-DriverRepo-OTA-ja.md`。コントロールセンターは、このリポジトリの最新のリリースの `manifest.json` を読んで、入っているドライバーと版を比べる。更新を入れる前に、次の3つを確かめる。
- HTTPS で取ってきたものか
- SHA-256 が合っているか
- テスト署名の証明書で署名されているか

インストールは、管理者の確認を出してから行う。管理者として起動し直したコントロールセンターが、同じリリースを GitHub から自分で取り直して、上の3つを確かめてから `pnputil` で入れる。

使い方: コントロールセンターの「ドライバー」タブで「更新を確認」→ 青い行（更新できるもの）を見て「更新する」。コントロールセンター v0.5.0 以降で使える。

GPIO と I2C は、ほかのドライバーが使っているので、その場では入れ替えない。Windows に登録して、次に起動したときに入れ替える（v0.5.1 以降）。

コントロールセンター自身も、新しい版があれば同じタブから更新できる（v0.5.1 以降）。ZIP の SHA-256 を確かめてから、自分を上書きして起動し直す。

新しいデバイスのドライバーも、コントロールセンターを直さずに配れる（v0.5.2 以降）。コントロールセンターは、manifest の各ドライバーの次の項目を使う。
- `hwid`: 見に行くデバイス
- `nameJa`、`nameEn`: 表示する名前
- `installAt`: `boot`（次の起動で入れ替える）か `live`（その場で入れる）
- `minUefi`: デバイスが無いときに、必要な UEFI の版として表示する

新しいドライバーを足すときは、`release/Make-Release.ps1` の表に1行足す。

## フォルダー

| 場所 | 内容 |
|---|---|
| `drivers/` | 各ドライバーのソースと INF |
| `kits/` | Switch で使う導入スクリプトと README（バイナリはリリースの ZIP に入る） |
| `app/SwitchCenter/` | コントロールセンター（.NET 8、WinForms、win-arm64） |
| `tools/` | `Build-Driver.ps1`（clang-cl + lld-link で KMDF をビルドし、テスト署名する）、テスト証明書の公開鍵 |
| `release/` | `Make-Release.ps1` |
| `docs/` | 設計のメモ |

## ライセンス

GPL-2.0（Windows-for-Switch と同じ）。レジスタの手順の一部は、Hekate と Linux（どちらも GPL-2.0）に従っています。
