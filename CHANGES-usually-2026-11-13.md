# winx `usually` 修正版 (2026-11-13)

ベース: steck0714/winx `usually` (b6be5f6)。**ビルド・実機テストは未実施**（ソース/ドキュメントの取り込みのみ）。

## 追加・更新 (WindowsDrivers/t210/Icosa)
- `drivers/SwitchWlan` (WiFiCx 0.6.13) と `kits/Switch-Wlan`：Erista向けWi-Fi。WDK 10.0.26100.0 が必要。ビルドは `tools/Build-Driver-WiFiCx.ps1`。
- `drivers/SwitchWifiProbe` (旧 switchwifi 0.4.0 を `switchwprobe` に改名) と `kits/Switch-Wifi-Probe`：Lite用 `SwitchWifi` 0.5.x と名前・HWID衝突するため分離。
- `drivers/SwitchAudio`, `drivers/Tegra210Sdhc`：hayato41200 の 2026-11-12 版に更新。
- `app-hayato/`：SwitchCenter / SwitchPatcher（既存 app とは混ぜない）。
- `docs/Erista-and-Lite-Wifi.md`
- NOTICE.winx.md / README.md のクレジットに参照元8リポジトリを追加。

## 注意
- `kits/Switch-Wlan/Wlan.ps1` は Lite用 switchwifi 0.5以降を消さない作り。ただし Install は同じデバイス用ドライバを入れるため、Lite機では実行しないこと。
- Lite用 t214/Hoag は下の「Lite追加更新」の範囲だけ更新（WinX側が新しい部分は据え置き）。
- bl33.bin は再ビルドしていない（edk2 サブモジュール・ARM64ツールチェーンがこの環境にないため）。
- ios-on-switch-lmao / windows-switch / mesa-switch-woa からのコードコピーはなし（参照のみ）。

## Lite (t214/Hoag) 追加更新 — ruje0504-2/Windows-for-Switch-Drivers `hdh001-winarm-bounded-write` (6cd7305, 2026-11-12) より
- `SwitchWifi` 0.5.5 -> 0.5.7（WPA2混在グループ鍵、再接続時のRX寿命、ファームのリンク状態確認、テスト2本追加）
- `Tegra210Sdhc` -> SD 0.5.1.2（DDR200チューニング書込みのコミット、フォールバック後の証拠保持、有界PIOクラッシュダンプ）。WinX側は上流の過去コミット 99f1b18 と完全一致していたため安全に置換。
- `SwitchFan` 1.0.0.11 -> 1.0.0.13（45℃でPWM全開の早期冷却カーブ、Lite実行時CPU目標1497.6MHz）
- `docs/hdh001/README.md`、`kits/Switch-Sd/SDLOADAA64*.EFI`、`kits/Switch-Stage0/SwitchWOA-Test.cer` を同期
- 据え置き（WinX側が上流より新しいため）: `SwitchPowerButton` 1.0.0.5 / `SwitchLiteTouch` 1.0.1.2 / `SwitchSensor` 0.2.5.0 / `TegraGpu`
- 確認済み: ATFパッチ 0005(BL33 SPSR)・0006(再起動エントリ) は既にWinXに入っている
- 注意: Erista用 `t210/.../switchwifi-erista.inf` は 0.5.5.0 表記のまま（共有コアは更新済み）。ビルド・実機は未検証。

## 追加取り込み（機種を問わず使えるもの）— 詳細は INTEGRATION-MAP-ja.md
- 適用: DanilKhmyrov の bootopt パッチ（Windows Boot Manager の登録）
- 追加: WindowsSetup/、third_party/(Danil, ios-on-switch-lmao)、WindowsDrivers/gpu/mesa-switch-woa/(WDDM/tgd3d/NVKのサブセット)、experimental/sc7-test/、Drivers/WifiBtApp/t214/power.h
- 除外: NVIDIA XUSBファーム、unifiedの bl33.bin、Mesa本体

## ファームウェア分離（bl33.bin と NVIDIA XUSB ファームを別ファイルで扱う）
- 既定で埋め込まない（NintendoSwitch.dsc の EMBED_XUSB_FIRMWARE=FALSE、元から）。UsbPowerAuditDxe は SD/ESP から実行時に読む。
- 修正: `Tools/build-woa.sh` が埋め込みなしでもファーム必須で止まっていた → `-D EMBED_XUSB_FIRMWARE=TRUE` のときだけ必須に。
- 追加: 探索パスを4つに（`\tegra21x_xusb_firmware` / `\switchroot\winx\...` / `\SwitchWOA\Usb\...` / `\xusb.bin`）。ヘッダ検証は従来どおり必須。
- 更新: Firmware/README.md、NintendoSwitch.fdf と Tools/build.sh のコメント。
- Wi-Fi/BT(BCM4356)のファーム・NVRAM・CLM・HCDは元からファイルパス指定で実行時ロード。
- 未検証: Drivers/UsbPowerAuditDxe の変更はビルドしていない（この環境にEDK2/ARM64ツールチェーンなし）。
