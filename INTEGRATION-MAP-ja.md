# 取り込み一覧と使い道（2026-11-13）

ビルド・実機テストは未実施。「適用済み」以外は、参照用またはそのまま使える補助ツールとして置いてあります。

| 場所 | 出どころ | 状態 | 使い道 |
|---|---|---|---|
| `Library/PlatformBootManagerLib/PlatformBm.c` + `patches/danil-bootopt-register-windows-bootmgr.patch` | DanilKhmyrov | **適用済み**（ARM64限定） | Windowsセットアップが「ブート構成を更新できない」で止まる問題の対策。パーティション付きの完全なデバイスパスで Windows Boot Manager を登録し BootOrder 先頭に置く。機種非依存。 |
| `WindowsSetup/` | steck0714 unified | そのまま使える | ログオン不具合の診断/修正、ドライバー棚卸しのPowerShell。 |
| `WindowsDrivers/t214/Hoag/drivers/{SwitchWifi,Tegra210Sdhc,SwitchFan}` | ruje0504-2 | 置換済み | Wi-Fi 0.5.7 / SD 0.5.1.2 / ファン 1.0.0.13（Lite）。 |
| `WindowsDrivers/t210/Icosa/drivers/{SwitchWlan,SwitchWifiProbe}` ほか | hayato41200 / steck0714 | 追加済み | Erista向けWi-Fi(WiFiCx 0.6.13)、Audio/SDの更新。 |
| `Drivers/WifiBtApp/t214/power.h` | steck0714 winx | 追加のみ（未配線） | BCM4356用PCIe電源(MAX77620 LDO1 1.0V)のポリシー。WifiPcie側から呼ぶ必要あり。 |
| `WindowsDrivers/gpu/mesa-switch-woa/` | ruje0504-2 | 参照/実験 | GM20B用WDDM(KMD)・tgd3d・NVKのWindows層。完成版ではない（デスクトップは短時間のみ）。機種はLiteで確認、T210(Erista)向けは未確認。 |
| `third_party/DanilKhmyrov-windows-switch/` | DanilKhmyrov | 参照 | EL2シム(PSCI/SDのstage-2トラップ)、swbatt/swhid、`desktop/`のカード作成・BCD生成スクリプト。25パッチのうち適用できたのは上の1件のみ（残り24はWinXが別系統で、そもそも当たらない）。 |
| `third_party/ios-on-switch-lmao/` | brunostjohn | 参照 | XUSBホスト(`SwitchUSB/XusbHost.h`等)・BPMP・DFLL・SMMU・EMC・PD・DPのレジスタ手順。`TegraXusb`やxHCI対応の突き合わせ用。IOKit/Bazel依存のため直接ビルドは不可。NVIDIAのXUSBファームは削除済み。 |
| `experimental/sc7-test/` | ruje0504-2 sc7-test | 実験（既定に混ぜない） | SC7(ディープスリープ)のATFパッチ3本とテスト。番号はusuallyの0005/0006と衝突するため `sc7-` 接頭辞にしてある。 |

## 入れなかったもの
- `steck0714/winx` `unified` の `release/bl33.bin`：usuallyのソースと対応が取れないため。
- ruje側 `main`/`docs/*` ブランチ：取得できない、またはドキュメントのみ。
- Mesa本体(約500MB)・iOS側のMetal/GPUコーパス(数MB〜数十MB)・Apple固有部分。
- NVIDIA製ファームウェア（XUSB）などのバイナリ。

## 次にやるとよいこと（未実施）
1. `bl33.bin` の再ビルド（bootoptパッチ込み）→ 実機でWindowsセットアップ後のBoot Manager登録を確認。
2. `third_party/ios-on-switch-lmao/drivers/SwitchUSB` のXUSB手順と `TegraXusb/src/mailbox.h` を突き合わせてxHCI側の抜けを洗う。
3. Erista用 `switchwifi-erista.inf` の版表記(0.5.5.0)を共有コア(0.5.7相当)に合わせて確認。
