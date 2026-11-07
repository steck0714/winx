# Windows for Switch

![Windows for Switch](docs/images/social-preview.png)

初代 Nintendo Switch（Tegra210「Erista」）で Windows 10 on ARM を動かすプロジェクトです。
- **対象の本体:** RCM に入れられる未対策の本体だけです。
- **Windows:** Windows 10 IoT Enterprise LTSC 2021（build 19044、ARM64）を使います。
- **起動のしかた:** coreboot と EDK2 の UEFI を使い、microSD から起動します。

このリポジトリは、[imbushuo/NintendoSwitchPkg](https://github.com/imbushuo/NintendoSwitchPkg)（EDK2 のプラットフォームパッケージ）の続きとして作っています。元の履歴は、そのまま残しています。このリポジトリで足したものは次のとおりです。

- USB、電源、画面などを Windows に引き渡すための UEFI の変更
- Switch のハードウェア向けの Windows カーネルドライバー一式
- 立ち上げの記録

English: [README.md](README.md)

> **注意:** 実験的なものです。検証の足りない電源・クロック・画面の処理を、実機で動かします。eMMC には一切書き込みませんが、自己責任で使ってください。CPU のプロファイルを標準より上げると VDD_CPU の電圧も上がるので、これも自己責任でお願いします。

## 動いているもの

| 機能 | 実現方法 |
|---|---|
| microSD からデスクトップまで起動 | RCM → coreboot → UEFI（このパッケージ）→ SD 上の Windows Boot Manager |
| USB ホスト（HS/FS）: ハブ、キーボード、マウス、ストレージ、RTL8153 の有線 LAN | UEFI が XUSB を起動したまま引き渡し、Windows 標準の `USBXHCI` が動かす |
| Windows の起動後の USB-C の抜き差し（機器なら 5V を出す・止める。充電器付きのハブなら充電 + ホスト） | UEFI R378 が、何もつながっていなくても XUSB を起動する。`switchbattery.sys` v4 が BM92T を見張る |
| USB-C PD のハブを通した充電 | UEFI: BM92T をシンクにし、BQ24193 を充電モードにして、DR_SWAP でホストになる |
| 起動時の CPU 1020 MHz、Windows 上で 614〜1766 MHz を選べる | UEFI（PLLX + MAX77621）+ `switchcpu.sys` + 常駐アプリ |
| ファン（温度による自動制御） | UEFI + `switchfan.sys` |
| 画面の明るさ。Windows が画面を消すとバックライトも消える | UEFI R376（PWM0）+ `switchfan.sys` v2 + 常駐アプリ |
| 音量ボタン | DSDT の `ACPI0011` + GPIO ドライバー + Windows 標準の `hidinterrupt` |
| 電源ボタン（画面オフ、ロック） | `switchpwrbtn.sys`（MAX77620 の EN0） |
| バッテリー残量、AC、充電中の表示 | `switchbattery.sys`（MAX17050 + BQ24193） |
| タッチパネル（マルチタッチ、長押し） | UEFI が FTM4 の電源を入れる + `switchtouch.sys` |
| 再起動しても時計が合っている | `switchrtc.sys` が MAX77620 の RTC を読み、最後の時刻同期で覚えたずれを足す。RTC には書き込まないので、Horizon の時計には影響しない |

**まだできないこと:**
- GPU による高速化（今は標準の画面ドライバーで描いている）
- 音声
- Wi-Fi と Bluetooth
- Joy-Con のレール
- スリープ

## 動作を確かめた OS

- Windows 10 IoT Enterprise LTSC 2021（ビルド 19044）ARM64

Windows のイメージとライセンスは、各自で用意してください。このリポジトリには、Windows は含まれていません。

## フォルダーの構成

| 場所 | 内容 |
|---|---|
| `AcpiTables/`、`Drivers/`、`Library/`、`NintendoSwitch.dsc/.fdf` | EDK2 のパッケージ。`Drivers/UsbPowerAuditDxe` が本体の立ち上げを行う。USB/XUSB、PD と充電、CPU のクロック、ファン、バックライト、タッチの電源を担当する。 |
| `Firmware/` | XUSB のファームウェアを置く場所。リポジトリには入っていない（README を参照）。 |
| `patches/edk2/` | edk2 の `XhciDxe` の修正。非コヒーレントな DMA バッファを write-combine で確保する。 |
| `patches/coreboot/` | [imbushuo/Coreboot](https://github.com/imbushuo/Coreboot) を、今のツールでビルドするための修正。 |
| `Tools/uefi/package-R378.sh` | 試験済みのイメージを作ったスクリプト（パスは元の環境のもの）。 |
| `Tools/rcm-mac/` | macOS から shofel2 でペイロードを送る手順。 |
| `windows/` | Windows を用意するときの道具（ログインの修正、ドライバーの棚卸し）。ドライバー本体は [Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers) にある。 |
| `docs/ja/uefi-revisions/` | UEFI の試験イメージごとの説明と、実機での結果。 |

## UEFI のペイロードのビルド

1. edk2 の `edk2-stable202102` を取ってきて、`patches/edk2/*.patch` を当てる（`patch -p1`）。
2. このリポジトリを `edk2/NintendoSwitchPkg` に置く。XUSB のファームウェアを、`Firmware/README.md` のとおりに置く。
3. `NintendoSwitchPkg/NintendoSwitch.dsc` を、AARCH64・GCC5 でビルドする。
4. `TEGRA210_EFI.fd` を ELF のペイロードに包んで、coreboot の ROM に `fallback/payload` として入れる。
   - coreboot の ROM は、imbushuo/Coreboot に `patches/coreboot` を当ててビルドしたもの。
   - 正確なコマンドは `Tools/uefi/package-R378.sh` にある（`objcopy` + `FvWrapper.ld` + `cbfstool`）。
5. Switch を RCM にして、shofel2 で ROM を送る。
   - Linux から: `shofel2.py cbfs.bin coreboot.rom`
   - macOS から: `Tools/rcm-mac/` を参照。

## Windows のドライバーの入れ方

ドライバー、導入キット、コントロールセンター（SwitchCenter）は、[Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers) に移しました。
そちらのリリースの ZIP を取ってきて、README の手順で入れてください。順番は、テスト署名の準備、GPIO、I2C、バッテリー、ファン、タッチ、電源ボタン、CPU クロック、時計（RTC）です。入れたあとは、コントロールセンターからドライバーを更新（OTA）できます。

## クレジット

- [imbushuo](https://github.com/imbushuo): NintendoSwitchPkg と、Switch 用の coreboot
- [CTCaer / Hekate](https://github.com/CTCaer/hekate) と Switchroot（L4T）: ファン、タッチパネル、バックライト、PMIC、充電 IC のレジスタの手順
- Linux カーネルの Tegra 向けドライバーとデバイスツリー（CPU の CVB 表など）
- fail0verflow: shofel2

## ライセンス

GPL-2.0 です。NintendoSwitchPkg と同じで、`LICENSE` を参照してください。立ち上げのコードの一部は、Hekate と Linux（どちらも GPL-2.0）の手順に従っています。
