# Switch機種・SoC別ソース構成

SoCファミリーを上位に置き、実機の4モデルをコードネーム別のディレクトリに分けます。同じSoCを使う機種でも、基板、電源、GPIO、画面、ストレージ、無線の差があるため、機種プロファイルは統合しません。

| SoC系統 | コードネーム | 製品 | WinXの選択マクロ | 実装状況 |
| --- | --- | --- | --- | --- |
| `t210/` | `Icosa/` | 標準Switch、HAC-001（Erista） | `SWITCH_BOARD_ERISTA` | プロファイルのビルド確認のみ。現在のBL33での実機識別・bring-upは未確認。 |
| `t214/` | `Iowa/` | バッテリー強化版、HAC-001-01（Mariko） | `SWITCH_BOARD_MARIKO` | プロファイルのビルド確認のみ。専用bring-upは未確認。 |
| `t214/` | `Hoag/` | Switch Lite、HDH-001 | `SWITCH_LITE_TOUCH` | プロファイルのビルド確認のみ。別のWindows/ドライバー構成でのLite実績はUEFIタッチ実績を意味しない。 |
| `t214/` | `Aula/` | Switch有機EL、HEG-001 | `SWITCH_BOARD_OLED` | プロファイルのビルド確認のみ。専用bring-upは未確認。 |

Hekateのソース表記はT210とT210B01を使います。WinXの`t214/`は慣用のT214別名で、HAC-001-01 / HDH-001 / HEG-001を同一ボードとみなす意味ではありません。このSoC/製品対応はHekateのヒューズ分類と公開情報に基づくソース上の分類で、現在のWinXが4機種を実機で自動識別した結果ではありません。各ボードのプロファイルは `Platforms/Switch/t210/Icosa/` または `Platforms/Switch/t214/{Iowa,Hoag,Aula}/` に置きます。製品名別の互換ソースディレクトリは作らず、既存の`SWITCH_BOARD_*`マクロだけをビルド互換として保持します。

共通処理は `Platforms/Switch/BoardProfile.h` と `Drivers/WifiBtApp/WifiPrepareCommon.h` に置き、電源ポリシーは `Drivers/WifiBtApp/t210/Icosa/` と `Drivers/WifiBtApp/t214/Hoag/` に分けます。T210の手順をT210B01へ、またはHoagの電源・表示設定をIowa/Aulaへ暗黙に流用しません。`t214/uphy.h`はT210B01向けSoC共通のUPHY処理です。

共通Windows ACPIは `OperatingSystems/Windows/Acpi/`、Hoag固有のGPU/表示/SIO ACPIは `Platforms/Switch/t214/Hoag/Acpi/` に置きます。OS別ブート要素は `OperatingSystems/{Windows,Linux,iPadOS,Other}/Boot/` にまとめます。

## 検証範囲

2026-11-10に4つのAArch64 RELEASEプロファイル（Icosa/Erista、Hoag/Lite、Iowa/バッテリー強化版、Aula/OLED）のビルドが通りました。これはコンパイルとFV生成の確認だけです。別途行ったQEMU確認は、一般的なARM `virt`用の既成EDK IIファームウェアによるShell・コンソール・FATマッピングのみで、現在のWinXイメージはQEMU起動していません。実機で確認された旧WinX Erista経路とLite上のWindows/ドライバー結果は、[検証状況一覧](verification-status.md)に機種・範囲を分けて記録しています。数値のMMIO/IRQ/クロック値は、明示した実機readbackを除きソース由来の理論値です。
