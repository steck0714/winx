# UEFI前の入力・スクリーンショット

このページは、ソースに実装した経路と実機で確認できた結果を分けて記録します。コードがあることだけでは、どのSwitchでも動作する証明になりません。

## 入力経路

| 入力 | WinX UEFIでの経路 | 状態と制限 |
|---|---|---|
| USBキーボード | EDK II `UsbKbDxe` → `EFI_SIMPLE_TEXT_INPUT_PROTOCOL` → `ConIn` | ドライバーはFDFに含まれます。USBホストコントローラーも必要です。WOA構成はXUSB/xHCI、非WOAでは`SWITCH_USB=1`のTegra EHCI構成を使います。オーナー報告にはEristaでUSBキーボードを使った例がありますが、今回の変更を含む各モデル・各キーボードの再試験ではありません。NKROや独自レポートを使う機種は互換性を確認してください。 |
| 本体の音量ボタン／電源ボタン | `SidebandButtonDxe`が標準テキスト入力を公開し、音量上下を矢印、電源をEnterとして`ConIn`へ追加 | T210/T210B01のPX6/PX7とPMIC経路を使用します。今回のソース変更で追加した経路で、四モデルすべての実機確認は未完了です。 |
| タッチパネル | WinXメニューはAbsolute Pointerを使用。ドライバーはEFIアプリ向けにSimple Pointerも公開し、押している間の移動を相対入力として渡す | WinXメニューのタップ選択を実装しています。タッチをSimple Pointerだけで読むアプリでは、押下中の移動と左ボタンとして見えます。現行イメージでの実機確認は未完了です。 |
| USBマウス | EDK II `UsbMouseDxe` → `EFI_SIMPLE_POINTER_PROTOCOL`。`MouseCursorDxe`がGOPに矢印を描画 | カーソル描画はポインターが見つからない間250 ms、検出後20 ms間隔で確認します。入力がない間はカーソルを出しません。マウスを使う場合もUSBホストコントローラーが必要です。新しい経路は実機負荷試験をしていません。 |
| GRUBのキーボード | UEFI `ConIn`を使う。必要なGRUB構成では`usb_keyboard`モジュールを指定できる | GRUB標準のテキストメニューで使えます。 |
| GRUBのタッチ／マウスポインター | このWinXメニューのポインター処理とは別 | GRUB標準メニューのポインター操作は確認していません。タッチ対応をWinXのブートメニューからGRUBへ引き継ぐ実装もありません。 |
| Joy-Con | UEFI Bluetooth HID入力ドライバーなし | 未実装です。後述のBluetooth HCI診断コードは、接続・ペアリング・L2CAP/HID入力を提供しません。Windows上のJoy-Con/HIDドライバーとは別です。 |

USB HIDキーボードにはUEFIのコンソール入力を使い、GRUBでもファームウェアが公開した入力を利用できます。[EDK IIのUSBキーボードドライバー](https://github.com/tianocore/edk2/tree/master/MdeModulePkg/Bus/Usb/UsbKbDxe)、[GRUBの`usb_keyboard`モジュール](https://www.gnu.org/software/grub/manual/grub/html_node/usb_005fkeyboard_005fmodule.html)を参照してください。UEFIはタッチ向けAbsolute Pointerと相対ポインター向けSimple Pointerを別プロトコルとして定義しています（[UEFI仕様](https://uefi.org/specifications)）。

### スクリーンショット

WinXのブートメニューに「Save screenshot to removable FAT storage」を追加しました。選択すると、その時点のGOP画面を24-bit BMPにして、最初に見つかった書き込み可能なリムーバブルSimpleFSボリュームの`\bootloader\screenshots\`へ保存します。ファイル名は`winxYYYYMMDD_HHMMSS-####.bmp`です。読み取り専用のEfiFsボリュームは保存先から除外します。これはWinX UEFI画面のキャプチャで、GRUB自身の画面を撮る機能ではありません。保存先がない、または書き込みに失敗する環境ではエラーを表示します。各Switchでのファイルシステム・容量・画面向きの実機確認は未完了です。

Hekate/Nyxと同じ`bootloader/screenshots`保存先を使います。Joy-Con Captureボタン（Nyxの割当てに相当）またはF12の要求経路もメニューにありますが、WinX側のBluetooth Joy-Con HIDドライバーはまだなく、Joy-Con操作は現状のソースだけでは動作確認できていません。

## 負荷・待ち時間の確認

- マウスカーソルはポインター未検出時に250 ms間隔（毎秒4回）で調べ、マウス検出後のみ20 ms間隔にします。カーソル描画は位置かボタンが変化したときだけです。無入力でも高頻度で回り続けるループはありません。
- タッチI2Cは無接触時20 ms、接触中10 ms間隔です。本体ボタンは20 ms間隔です。いずれもタイマー駆動で、ビジーループではありません。実機CPU負荷や電池消費は未測定です。
- WinXメニューはキー／ポインターのイベントを待ちます。タッチ選択後はリリースまで最大5秒ビジー待ちしていた処理をなくし、リリースイベントまで次のタップを無視する方式にしました。
- スクリーンショットは選択時だけ保存します。画面全体の約3.7 MiBのピクセルバッファを確保せず、32行単位の約280 KiBバッファで変換し、720行の個別書き込みを最大23回のまとまった書き込みに減らしました。低速カードでは保存中に待ちが出る可能性がありますが、通常のブートやメニュー表示では動きません。
- 上記はコード経路からの負荷確認で、実機ベンチマークではありません。WOAのUSB/XUSB bring-upや`SWITCH_LITE_AUDIO`のcodec初期化には、別途ハードウェア待ちとブロッキング処理があり、このポインター負荷とは別に確認が必要です。

## コードネーム別の見方

| SoC / プロファイル | コードネーム | 入力上の注意 |
|---|---|---|
| T210 | Icosa（Erista） | USBホストはビルド設定に依存します。本体ボタンはGPIO/PMIC初期化が合うか実機確認が必要です。 |
| T210B01 | Hoag（Lite） | タッチパネルはFTM4経路です。Windowsのタッチ実績をUEFIの実績として数えません。 |
| T210B01 | Iowa（バッテリー強化版） | Hoagと同じSoCでも、ボード周辺回路・パネル構成は別途確認が必要です。 |
| T210B01 | Aula（有機EL） | Hoagと同じSoCでも、ボード周辺回路・パネル構成は別途確認が必要です。 |

確認結果を追加するときは、使用したBL33/FV、コードネーム、USBアダプター、キーボード・マウス型番、タッチ画面の向き、試した操作を記録します。現在の検証区分は[verification-status.md](verification-status.md)を参照してください。
