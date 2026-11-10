# UEFIの音声・Wi-Fi・Bluetooth

## 音声出力

Tegra210のAHUB/ADMA/I2S1とRealtek ALC5639 codecを使う既存bring-up経路を調べ、`WINX_AUDIO_OUTPUT_PROTOCOL`を追加しました。現在の形式は48 kHz、ステレオ、16-bit signed little-endian PCMです。`WritePcm16`は最大480 frames（10 ms）を同期再生し、UEFIアプリが小さな音声ブロックを順番に渡せます。`Stop`はDMAチャンネルを停止します。UEFI仕様の標準サウンドAPIではなく、WinX固有のプロトコルです。

プロトコルを公開する構成は、既存のaudio bring-upを含むWOA構成と、`SWITCH_LITE_AUDIO=1`で`LiteAudio.inf`を含めるHoag構成です。初期化時に4 KiBの低位DMAページを一度だけ確保して再利用します。既存コードには1 kHzの短い診断音を物理スピーカーで聞けたという開発記録がありますが、この新しいPCMプロトコルはまだ実機検証していません。現時点でDoomの移植や音声出力を完了扱いにはしません。

Doom側のアダプターは、DXEプロトコルを見つけてPCMブロックを渡す必要があります。DMA呼び出しは最大10 msの同期処理です。ゲーム側からは10 ms以下の小さなPCMブロックで渡し、描画ループで待ち時間が許容できるか測定してください。サンプルレート変換、ミキサー、音量API、非同期リングバッファ、ホットプラグやOSへの引き継ぎは未実装です。既存のWindows `switchaudio.sys`はWindows用で、UEFIアプリがこのAPIを使う代わりにはなりません。

## Wi-Fi

Switch内蔵のBroadcom BCM4356はWi-FiとBluetoothのコンボチップです。WinXの`SWITCH_WIFIBT=1`でビルドされる`wifibt`は、PCIe/ファームウェア準備と受動スキャンを調べる診断コードです。`EFI_SIMPLE_NETWORK_PROTOCOL`を公開せず、アクセスポイント認証・接続・IP取得まで行うUEFI Wi-Fiドライバーではありません。スキャン表示が出ても、UEFIからネット接続できることを意味しません。

`SWITCH_NET=1`のEDK IIネットワークスタックとCDC-ECM/NCM/RNDISクラスドライバーは、対応する有線USB NICやUSBテザリング用です。これらは内蔵BCM4356のドライバーではありません。T214 HoagのWindows Wi-FiドライバーもWindows NDIS/KMDF用です。UEFI Wi-Fiを完成させるには、PCIeとチップ電源の初期化、BCMファームウェアとNVRAM、デバイスコマンド、802.11接続・認証、SNPへのパケット統合まで別に実装・検証する必要があります。

ARM64 Linuxでは、Switchroot/L4Tのカーネル側`brcmfmac`と対応ファームウェアが別の実装経路です。[Linux WirelessのBroadcomドライバー資料](https://wireless.docs.kernel.org/en/latest/en/users/drivers/brcm80211.html)を参照してください。LinuxドライバーやWindowsドライバーのコードをUEFIドライバーとして直接リンクすることはできません。

## BluetoothとJoy-Con

UEFIコードにはUART-D経由のHCI電源投入・初期化とスキャン診断があります。これは完全なBluetoothホストスタックではありません。ペアリング、L2CAP、HID over GATT/HIDP、入力レポートをWinXメニューへ渡す仕組みがないため、Joy-ConをUEFIメニューで使う機能は未実装です。WindowsのBluetoothスタックとWindows用ドライバーは、UEFI段階の入力経路にはなりません。

Joy-Con対応には、HCI transport、コントローラー固有の初期化・接続、必要な暗号化/ペアリング手順、HID入力レポート処理、標準EFI入力へのマッピングと切断回復を追加して実機確認する必要があります。USBキーボード・マウスはBluetoothとは別で、[UEFI入力資料](preboot-input-ja.md)のUSBホスト条件に従います。

## 次に埋める差分

1. `WINX_AUDIO_OUTPUT_PROTOCOL`の小さなテストアプリを作り、HoagとWOA対象機でPCM連続ブロック、音量、ヘッドホン切替、Windows/Linux起動への影響を確認する。
2. Iowa/Aulaを含む各ボードの音声codec、pad/clock/power状態を照合し、別プロファイルを追加する。
3. Wi-Fiの目標がUEFIネット接続なら、`EFI_SIMPLE_NETWORK_PROTOCOL`を前提にPCIe firmware bring-upから接続・DHCPまで段階的に実装する。診断用スキャンをそのままネットワーク機能と呼ばない。
4. Joy-ConはBluetooth HCI/HIDスタックの段階を分け、USB入力や本体ボタンを壊さない形で追加する。

現在のコードと検証状況は[verification-status.md](verification-status.md)にも反映します。
