# ドライバーの別リポジトリと OTA 更新：設計

作成: 2026-10-26（Claude）

## 目的

- **別リポジトリ:** Windows のドライバーを、UEFI（`Windows-for-Switch`）とは別の GitHub リポジトリで管理する。
- **OTA 更新:** コントロールセンターから、新しいドライバーの確認とインストールができるようにする。

## 1. 別リポジトリ

- **名前（案）:** `Windows-for-Switch-Drivers`。ブラウザで、空の Public リポジトリとして作ってもらう。
- **中身:**

| 場所 | 内容 |
|---|---|
| `drivers/<name>/src`、`<name>.inf` | 各ドライバーのソース（GPIO、I2C、バッテリー、ファン、タッチ、電源ボタン、CPU、RTC） |
| `kits/Switch-*` | 今の導入キット（スクリプトと README） |
| `tools/` | `Build-Driver.ps1`、`kmdf_support.c`、テスト署名の証明書（公開鍵だけの .cer） |
| `app/SwitchCenter/` | コントロールセンター |
| `release/Make-Release.ps1` | 全部をビルドして、配布用の ZIP と `manifest.json` を作る |

- **`Windows-for-Switch` との関係:** `Windows-for-Switch` には、UEFI と DSDT を残す。`windows/` は、新しいリポジトリへの案内に置き換える。

## 2. 配布の形（GitHub Releases）

1つのリリース（例: `drivers-2026.10.26`）に、次のファイルを付ける。

| ファイル | 内容 |
|---|---|
| `SwitchDrivers-<版>.zip` | 全ドライバーの .sys / .cat / .inf（テスト署名済み） |
| `SwitchCenter-<版>-arm64.zip` | コントロールセンター本体 |
| `manifest.json` | 各ドライバーの名前、ハードウェア ID、版（DriverVer）、ZIP の SHA-256、必要な UEFI の版 |

`manifest.json` の例:

```json
{
  "release": "drivers-2026.10.26",
  "packages": [
    { "file": "SwitchDrivers-2026.10.26.zip", "sha256": "…" },
    { "file": "SwitchCenter-0.4.0-arm64.zip", "sha256": "…" }
  ],
  "drivers": [
    { "service": "switchbattery", "inf": "switchbattery.inf", "hwid": "ACPI\\SWOA0301", "version": "1.0.0.5", "minUefi": "R378" },
    { "service": "switchcpu",     "inf": "switchcpu.inf",     "hwid": "ACPI\\SWOA0701", "version": "1.0.0.2", "minUefi": "R375" }
  ]
}
```

- **リリースの作り方:** `Make-Release.ps1` が、ZIP と `manifest.json` を作る。
  - アップロードは、ブラウザで GitHub のリリース画面にドラッグする。PC に GitHub CLI を入れれば、それも自動にできる。
  - 署名の秘密鍵は PC にしか無いので、ビルドとリリースの作成は PC で行う。

## 3. OTA（コントロールセンターの「ドライバー」タブ）

1. **確認:** `https://api.github.com/repos/<owner>/Windows-for-Switch-Drivers/releases/latest` を読み、`manifest.json` を取ってくる。
2. **比べる:** 入っている各ドライバーの版を、Windows の情報（Win32_PnPSignedDriver の DriverVersion）から読み、manifest の版と比べる。更新があるものを一覧にする。
3. **ダウンロード:** ZIP をダウンロードして、SHA-256 を manifest の値と照らし合わせる。合わなければ、何もしない。
4. **インストール:** ドライバーのインストールには管理者の権限が要る。そのため、コントロールセンターを管理者として起動し直して（UAC の確認が出る）、`pnputil /add-driver <inf> /install` を、更新のあるものだけに行う。
5. **確かめる:**
   - インストールの前に、各 .cat の署名が、決まったテスト証明書（thumbprint `A9E1F054…`）で行われていることを確かめる。
   - 必要な UEFI の版（`minUefi`）より古い ROM で起動しているときは、インストールしない。今の ROM の版は、SMBIOS か DSDT のデバイスがあるかで判断する。
   - インストールのあと、デバイスの状態（問題コード）を表示する。再起動が必要なら知らせる。
6. **コントロールセンター自身の更新:** 同じ仕組みで、新しい版の ZIP を取ってきて、自分を閉じてから入れ替える。小さな補助の exe を使う。

**安全のための考え方:**
- インストールするのは、自分のリポジトリのリリースだけ。どのリポジトリから取るかは、アプリに固定で書く。
- 確かめるのは、HTTPS、SHA-256、署名の証明書の3つ。
- 勝手には入れない。必ず、一覧を見せてから「更新する」を押してもらう。

## 4. 段階

| 段階 | 内容 |
|---|---|
| A | コントロールセンターの段階3（電源・ドライバーの状態・記録のタブ）。ドライバーのタブは、まず「入っている版と状態」の表示だけ |
| B | 別リポジトリを作って、ソースを移す。`Make-Release.ps1` と最初のリリース |
| C | ドライバーのタブに「更新を確認」「更新する」を足す（OTA）。**v0.5.0 で作った**（2026-10-26）。UEFI の版は DSDT のデバイスから推定する |
| D | コントロールセンター自身の更新。**v0.5.1 で作った**（2026-10-26）。補助の exe は使わず、展開した新しい版が `--self-update` で古い版を上書きする |

## 決めてほしいこと

- **リポジトリの名前:** `Windows-for-Switch-Drivers` でよいか。
- **リポジトリの作成:** 段階 B の前に、ブラウザで空のリポジトリを作ってもらう。
