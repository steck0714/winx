# WinX ドライバー不足点検

2026-11-10 時点で、作業ツリーのソース、付属の機種別 WindowsDrivers、記録済みの実機報告、EDK II / Hekate / Switchroot / Mesa の公開ソースを横断して確認した範囲です。**ファイルがあること、ビルドできること、別プロジェクトで動いたこと、WinX の現行イメージで各機種に動いたことを同じ扱いにしません。**

## 状態の読み方

| 表記 | 意味 |
|---|---|
| 実機報告 | 指定された機種と版で所有者・開発者から報告された結果。独立再現とは限らない |
| ソース / ビルド確認 | コード、設計、ホスト試験、またはビルドの結果。Switch 実機で動く証明ではない |
| 未検証 | 対象の機種・ファームウェア・ドライバーの組合せで動作結果がない |
| 未実装 | 機能を提供するコード経路そのものがない |

## UEFI / BL33

| 機能 | 現在のコードと確認状況 | 残っている作業 |
|---|---|---|
| 画面・横向きメニュー | `SimpleFbDxe` がGOPフレームバッファを公開します。標準ビデオ経路は3D GPUではなく表示面です。`SwitchBootMenu.c` はカテゴリ切替付きの1列一覧、選択パス、診断画面を持ちます。文字の形とサイズは現在UEFIコンソールが提供します。 | 今回のソース変更後のAArch64ビルドと4モデル実機表示確認。固定幅文字、タッチ座標、壁紙の有無を確認。独自フォントはGOP文字レンダラーを追加する段階で検討。 |
| USBキーボード / USBマウス | EDK II `UsbKbDxe` / `UsbMouseDxe` をFDFに含める構成があります。EDK IIキーボード実装はHID Boot Protocol前提です。古いEristaで1台のUSB 2キーボードが動いたという報告はありますが、現行FV・各機種での広い再確認ではありません。 | USBホストと電源、ハブ、複合デバイス、NKRO / Report Protocol専用キーボード、配列、スリープ復帰を機種別に確認。USB-Cアダプターも記録。 |
| タッチ | `TouchPanelDxe` はAbsolute Pointerを公開し、Simple Pointer経路も持ちます。WinXメニューはタップ選択に対応するコードです。Windowsのタッチ実績はUEFIの実績ではありません。 | 現行FVを各パネルで試し、座標・回転・押下/解除・連打と長時間待機を確認。Hoag用FTM4設定をIowa/Aula/Eristaに流用しない。 |
| 本体ボタン | `SidebandButtonDxe` が音量と電源ボタンを標準キーへ変換します。今回の変更を含む4モデルでの実機確認は未完了です。 | GPIO/PMIC経路とキーリピートをモデル別に確認。 |
| Joy-Con | メニューは任意の `WinXJoyConInput` を読むコンシューマ側コードを持ちますが、プロトコルを公開するJoy-Conドライバーはありません。したがってUEFIメニューのJoy-Con操作は未実装です。 | レール電源・UART/I2C・コントローラー初期化・HIDレポート・切断再接続を含むプロデューサーが必要です。Hekateの[Joy-Conコード](https://github.com/CTCaer/hekate/blob/master/bdk/input/joycon.c)は豊富な実装参考ですが、単純なボタン変換だけでは代替できません。 |
| 内蔵Wi-Fi / Bluetooth | `WifiBtApp` はBCM4356のPCIe/ファームウェア準備・スキャン等の診断用コードです。EDK II標準のSNP Wi-Fiドライバー、AP接続、認証、DHCP、Bluetooth HID入力はありません。 | UEFIネット接続にはPCIe/電源、チップ firmware/NVRAM、データリング、802.11認証、SNPへの統合が必要。BluetoothにはHCIからL2CAP/HIDまで必要。 |
| USB Ethernet | `SWITCH_NET` はEDK IIのUSB CDC-ECM/NCM/RNDISとIPv4等の汎用ネットワークモジュールを追加できます。 | 対応USB NICを接続したSwitch実機でSNP、DHCP、TFTPを個別確認。これは内蔵BCM4356の代わりではありません。 |
| XUSB / xHCI | `SWITCH_XUSB` の `XusbInit` はDSCにある実験アプリ/シェルコマンドです。FDFに組み込まれず、汎用の完全なXUSBホストコントローラードライバーではありません。 | 適切なXUSB初期化とEDK II xHCIホストの統合、DMA/割り込み/ポート電源の確認。T210/T214ごとにACPI・電源条件を分ける。 |
| 音声 | Hoag向けの任意DXE `LiteAudio` とPCM16のWinX独自プロトコルがあります。UEFI標準の音声サービスではなく、新しいPCM経路の実機確認もありません。 | ALC5639 codec、DMA、スピーカー/ヘッドホン、停止/起動引継ぎを各機種で確認。UEFI Shellアプリ向けのテストプレイヤーは別途必要。 |
| EfiFs | 汎用EfiFsは読み取り用として使います。 | 読み取り対応ファイルシステムの組合せは全件実機確認されていません。EfiFs経由で書き込めるとは扱いません。 |
| 診断 / スクリーンショット | F10/Dの診断はプロトコル・Boot項目・ボードIDの存在を読むだけです。スクリーンショットは明示操作時にリムーバブル書込可能FATへ保存します。 | 診断のプロトコル件数は機能試験ではありません。画像サイズ・保存時間・ファイル完全性は各カードで確認が必要です。 |

## Windows ARM64

| SoC / コードネーム | 収録済みのドライバー範囲 | 主要な不足・制限 |
|---|---|---|
| T210 / Icosa / Erista | `WindowsDrivers/t210/Icosa/` にBattery、Bluetooth、CPU Clock、Fan、Power Button、RTC、Sensor、Touch、GPIO、I2C、SDHC、Audio等があります。今回、共通BCM4356 miniportを使うErista専用ACPI ID/INFと、明示opt-inのUEFI hand-offを追加しました。 | Erista Wi-Fiは**コード接続のみ・実機未検証**です。Pcie/UPHY、無線電源、割り込み/DMA、Erista用firmware/NVRAM、WindowsのWPA2/DHCPを確認する必要があります。機能は既定無効です。フルXUSBドライバーはありません。GM20B診断はS0–S2の範囲で、README上Windowsでロード未確認です。GPU WDDM/UMD/Vulkan ICD、実機での安定したSD速度、キーボードを含む現行WinXとの組合せも不足。 |
| T214 / Hoag / Switch Lite | `WindowsDrivers/t214/Hoag/` にWi-Fi、Bluetooth、Audio、Touch、SIO/gamepad、Battery、CPU Clock、Fan、Power Button、RTC、Sensor、GPIO、I2C、SDHC、TegraXusb、TegraGpu等があります。Wi-Fi WPA2/DHCP/Internet、スピーカー再生、タッチセットアップ等はソースリポジトリに所有者の実機報告があります。 | XUSBは物理確認待ち。GPUは試験KMD/ライブラリとプローブであり、完成したDisplay MiniportやVulkan ICDではありません。Bluetooth反復起動/スリープ復帰、ヘッドホン、充電の全サイクル、SDの連続スループット、バッテリー深いスリープは未完了です。 |
| T210B01 / Iowa | プラットフォーム識別・電源・ACPIの一部構成があります。WindowsDriversにIowa専用ドライバーフォルダーはありません。 | Hoagと同じSoC系列でも基板、電源、パネル、I2C接続とSD配線の一致は未証明。ドライバーをそのまま有効にせず、独立したACPI・電源・タッチ・Wi-Fi/BT・SD確認が必要です。 |
| T210B01 / Aula / OLED | プラットフォーム識別・一部ACPI構成があります。WindowsDriversにAula専用ドライバーフォルダーはありません。 | OLEDパネル、電源、GPU/DRAM carveout、USB-C、センサーの独立プロファイルと実機検証が必要。Hoag/Iowaの設定を流用できる根拠はありません。 |

Windows側の補助ドライバー（ファン、バッテリー、クロック、RTC、センサー、タッチ、ゲームパッド等）はすでに最低限を越えたソースがあります。今回追加したT210 Wi-FiはT214の物理結果を引き継がず、別のACPI IDと既定無効の機種別準備経路を持たせました。WiFiCxはMicrosoftの[NetAdapterCxベースのクライアントモデル](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/writing-a-wificx-client-driver)に従う必要があります。Windows用KMDF/SDPORT/WiFiCx/XUSBは、UEFI段階やLinux/iPadOSのドライバーにはなりません。Windowsソースの出典・ライセンスは[WindowsDriversの一覧](../WindowsDrivers/README.md)と[NOTICE](../NOTICE.winx.md)を参照してください。

## GPU: ゲームを動かせる段階まで

**技術的には目標にできますが、現状はゲーム用GPUドライバーではありません。** GOPの画面表示、GPUの電源/レジスタを読む診断、FIFOに限定コマンドを送る試験、オフスクリーンで三角形を描く試験、Windowsデスクトップやゲームで使えるドライバーはそれぞれ別の段階です。

このツリーのT214 GPUコードはKMD補助部品と専用プローブが中心で、READMEは完成したDisplay Miniport / Vulkan ICDではないとしています。`docs/gpu-vulkan/STATUS.md`の三角形/readback報告は別の `vulkan-test` 作業からの報告で、WinXの現行ファームウェアやDWM表示を確認したものではありません。T210のGM20B-DiagnosticはステージS0–S2のみで、WDDM経路はありません。

ゲームを実行するには、少なくとも次が揃ってからです。

1. 各ボードでGPU電源・クロック・リセット・割込み・メモリ/仮想アドレス・firmwareの契約を確定する。
2. Windows WDDM KMDを完成させる。T214にはVidMmの割当・ページングDDI、GPU仮想アドレス、フェンス、キュー、TDRの一部があり、Mesa側KMTには割当/常駐とVAのbind/unbindがあります。ただし実機のNVK実行は未確認です。DMA/コマンドのクラス別アドレス・権限・常駐検証、完全なプリエンプション/リセット・TDR・電源管理が残っています。
3. NVIDIA Maxwell向けのシェーダーコンパイラ/コマンド生成をWindows用ユーザーモードドライバーとして統合し、Vulkan ICDかD3D UMDを作る。
4. Windowsの表示経路/DWMへ安全に提示し、動画再生、長時間負荷、GPUハング回復を確認する。
5. 各コードネームごとに、小さなVulkan/Direct3Dアプリ、実ゲーム、温度/消費電力と復帰を段階確認する。

[steck0714/mesa-switch](https://github.com/steck0714/mesa-switch)は[danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch)のフォークです。2026-11-10に確認した時点では、フォークの`main`は親と同一で、独自コミットはありませんでした。READMEが説明するMesa 26.2.4移植はGM20B上のHorizon向けで、loaderless NVK Vulkan、Gallium NVC0 / Zink OpenGL、EGLを提供し、`src/nouveau/horizon`がlibnx経由でデバイス、アドレス空間、メモリー、チャネル送信、syncpoint同期、キャッシュ管理、エラー処理を担います。これはGM20Bを使った有用なユーザーモードGPU実装例ですが、Horizon専用バックエンドなのでWinXのUEFIやWindowsへそのまま組み込めません。Horizonのメモリー・キュー・同期方式はWindows WDDMと異なり、Microsoftの[WDDM構成](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/windows-vista-and-later-display-driver-model-architecture)ではWindows用KMDとUMDの両方が必要です。MesaのNVK対応GPU一覧にMaxwellが含まれることも、Switch上のWindowsで動く証拠にはなりません。従って最初の現実的な目標は「T214で限定的なオフスクリーンVulkanテストをWinXと独立して再現」し、その後にDWM/ゲーム対応を目指すことです。現時点でゲームをプレイ可能とは案内できません。

## microSD: 読み書きを速くしつつ読める状態を保つ

### 現在の状態

- **T210 Windows miniport:** `WindowsDrivers/t210/Icosa/drivers/Tegra210Sdhc/src/nvtsdhc.c` は既定でADMA2 32-bitを有効にし、3.3 V / High-Speedの保守的な経路を使います。UHS-I電圧切替とtuningは無効です。ソースコメントのPIO上限値は理論上の目安であり、T210実機の連続スループット測定ではありません。READMEにある「PIO_ONLY default on」は現在のソース既定値 `NV_T210_PIO_ONLY=0` と食い違っていたため、この監査で訂正対象にしました。T210 miniport全体はREADME上、実機未検証です。
- **T214 Windows miniport:** SDR104候補で電圧切替、CMD19 tuning、ADMA2を使います。HDH-001で199.68 MHz / tuning成功 / ADMAエラー0のドライバーreadbackと、1,867,466ブロック転送の記録があります。ただし独立した読み込み/書き込みMB/sベンチマークや長時間データ完全性試験ではありません。
- **T214 DDR200:** オプションビルドです。カード能力、最低8 tapの窓、ブートセクタreadbackを確認してから切り替え、失敗時はSDR104へ戻すコードがあります。0.5.0.3でfallback後の割り込みmask復元を直し、所有者はWindowsデスクトップ起動を確認しました。DDR200が実際に選択されたか、読み書きがどれだけ速くなったか、全カードで復旧するかは未確認です。速度既定値として有効にする根拠はまだありません。
- **境界保護:** T214のWINARMプロファイルはカード/パーティション情報を検査し、Windows書き込みを設定したWINARM範囲（または明示的に選ぶ追加FAT32範囲）に限定します。GPT/MBRと他パーティションを対象にしないコードがあり、WinX側のFAT32は別用途で読み込みます。プロファイル用ヘッダーとBL33 / miniportを混ぜると起動できない実例があるため、同一生成物を対で使います。

### 安全な速度向上の順序

1. 同一カード・同一温度・同一パーティション表示・同一BIOS/BL33で、現状のシーケンシャルread/write、ランダムread、エラー数、`BusSpeed`、`Tuned`、`AdmaErrors` を記録する。未測定のソース上の周波数をMB/sとして扱わない。
2. まずDMAバッファ再利用、複数ブロックの大きな要求、転送分割、割込み/PIO切替の測定を行う。電圧・クロックを変えないソフトウェア最適化の方が先です。
3. T210では現行48 MHz/ADMA2とPIOを同じカードで比べ、`NV_T210_PIO_ONLY=1` の復旧経路も残す。UHS-Iを試す場合はT210固有のpad/電源/tuningを測定してから別のopt-in構成にする。
4. T214では既存のSDR104を比較基準にし、まずカード上の専用テストファイルで読み書きと内容ハッシュを往復確認する。DDR200はカード能力・tap窓・復旧状態を読み返してから一枚ずつ試す。
5. 1件でもCRC/readback/timeout不一致が出たら高速モードを無効にして標準モードへ戻す。カード再認識まで普通のI/Oを許可しない。既存の境界検査・Windowsのブートファイル・FAT32可視設定は速度検討と同時に変更しない。

この順なら、**読み書き速度を上げる作業はできます**。ただし、現状のデータではT214 SDR104/ADMA2の連続read/write速度やDDR200実効速度を断定できず、T210も実機測定がありません。カード互換性を維持するには、速度モードの自動判定・検証・低速フォールバックを必須にして、全機種で一律の高速設定を強制しないことが重要です。

## 機種をまたぐ次の優先順位

1. Erista / Hoag / Iowa / Aulaそれぞれの`RELEASE AARCH64` FVを再ビルドし、容量と自己検査を記録する。今回のメニュー・タッチ・音声変更分はまだこの4ビルドで確認されていません。
2. 同一BL33でキーボード、画面、タッチ、マウスの無入力負荷、スクリーンショット、Shell黒画面を実機確認する。
3. SDはまず既知の起動できるSDR104/High-Speedを基準に、read/writeベンチマークとハッシュ検証を追加する。DDR200は既定無効を保つ。
4. HoagでGPUの限定プローブ/オフスクリーンテストを進め、WDDM、UMD、表示、DWM、実ゲームを別々の合格条件にする。
5. 他の3ボードはプロファイルごとに初期化・電源・ストレージ・パネルを検証してから、対応ドライバーを有効化する。

## 参考にした公開ソース

- [EDK II `UsbKbDxe`](https://github.com/tianocore/edk2/blob/edk2-stable202608/MdeModulePkg/Bus/Usb/UsbKbDxe/KeyBoard.c)
- [Hekate / Nyx](https://github.com/CTCaer/hekate) と [Joy-Con初期化コード](https://github.com/CTCaer/hekate/blob/master/bdk/input/joycon.c)
- [Switchroot T210/T210B01カーネルとSDHCI](https://github.com/CTCaer/switch-l4t-kernel-4.9/releases)
- [steck0714/mesa-switch](https://github.com/steck0714/mesa-switch) / [parent](https://github.com/danfromtico/mesa-switch) と [Mesa NVKの対応GPU](https://docs.mesa3d.org/drivers/nvk.html)
- [Microsoft WDDM architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/windows-vista-and-later-display-driver-model-architecture)
- [WinX T214 Windows driver fork](https://github.com/ruje0504-2/Windows-for-Switch-Drivers) と [WinX base repo](https://github.com/steck0714/winx)
