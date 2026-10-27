# winx への Windows-for-Switch(hayato41200)移植 — `SWITCH_WOA`

クレジット: **hayato41200**
- https://github.com/hayato41200/Windows-for-Switch
- https://github.com/hayato41200/Windows-for-Switch-Drivers

## 何を入れたか
| 場所 | 内容 |
|---|---|
| `Drivers/UsbPowerAuditDxe/` | 先駆者のボード初期化ドライバ(5100行)。XUSB ファームウェア読込と xHCI 起動、USB-C PD/充電、CPU クロック、ファン、バックライト、タッチ電源。**無改変**(先頭に出典コメントだけ追加) |
| `AcpiTables/Dsdt/Dsdt.asl` | XHC0 を先駆者の形に修正(`_CCA=0`、FPCI/IPFS、SPI 39/40/49)。`SWOA0101〜0801` デバイス群を追加 |
| `NintendoSwitch.dsc` / `.fdf` | 新スイッチ `SWITCH_WOA`。EHCI 系・TouchPanelDxe・FanDxe を外し、UsbPowerAudit + XhciDxe + XUSB ファームウェアを入れる |
| `patches/edk2/0001-*.patch` | XhciDxe の DMA バッファを write-combined に(edk2-stable202402 用に作り直し) |
| `WindowsDrivers/` | Windows 側ドライバ(KMDF、8種+診断)、SwitchCenter、署名・リリース用スクリプト |
| `WindowsSetup/` | ログイン修正、ドライバ一覧スクリプト |
| `Firmware/README.md` | XUSB ファームウェアの入手方法(バイナリは同梱しません) |
| `docs/hayato41200/` | 先駆者の README と R361〜R378 の検証メモ |
| `Tools/build-woa.sh` | ビルドスクリプト |

## ビルド
```
git clone --depth 1 --branch edk2-stable202402 https://github.com/tianocore/edk2.git
# サブモジュール(README.winx.md と同じ)を取得
# Firmware/tegra21x_xusb_firmware を置く(Firmware/README.md)
# Include/FwReleaseInfo.h を作る
Tools/build-woa.sh
```
Windows 側は `WindowsDrivers/` の各 `kits/Switch-*` と README の手順で、テスト署名 → GPIO → I2C → 各ドライバの順に入れます。

## ビルド確認(2026-10-28、この環境)
- edk2-stable202402 + GCC5/aarch64 で `-D SWITCH_HEKATE=1 -D SWITCH_WOA=1` のビルドが通り、`make-bl33.py` で bl33.bin まで生成できた。
- UsbPowerAuditDxe と XhciDxe が FV に入り、XUSB ファームウェア用の FREEFORM も入ることを確認。
- FVMAIN は 99% 埋まる(空き40バイト)。ただし従来の `SWITCH_USB=1` ビルドも 100% で、同じ状況。機能を足すなら `SWITCH_BIGFD=1` か不要ドライバの削除が必要。
- 従来構成(`SWITCH_HEKATE=1 SWITCH_USB=1`)も、同じ環境でクリーンビルドが通ることを確認(WOA を入れても壊していない)。
- 検証ではダミーのファームウェアを使った。実機用には本物が必要。

## 未検証・注意(実機)
- **hekate 経路での動作は未検証。** 先駆者は coreboot 経由です。UsbPowerAudit は「coreboot が残す状態」(PLLX=691.2MHz、XUSB のゲート/リセット、PMC のビットなど)を基準にし、違えば安全のため `STOP` して何も書きません。hekate が残す状態は違うので、最初は STOP する可能性があります。その場合、画面に出る `STOP:` の行が手がかりです。
- 画面は UEFI の Print に出ます。実行後に「POWER 12〜15秒で停止」と出る診断系の経路があります。意図しない停止に見えたら、まず出力を撮ってください。
- UEFI の USB キーボードなどは、EHCI から xHCI(UsbBus 経由)に変わります。winx 側の EHCI 経路(`SWITCH_USB`)とは併用できません。
- UEFI 内でタッチを使う `TouchPanelDxe` と、`FanDxe` は `SWITCH_WOA` では外しています(UsbPowerAudit が同じ I2C/PWM を使うため)。
- Windows 側の動作、SD への書き込みなどは、先駆者のコードの挙動に依存します。
