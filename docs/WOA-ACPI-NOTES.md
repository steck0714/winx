# WOA-Project 調査メモ (ACPI / Windows 起動)

調べた先: WOA-Project/Lumia950XLPkg, edk2-porting/edk2-sdm845, WOA-Project/mu_andromeda_platforms (2026-10-15 時点の HEAD)。

## 現状 (winx)
AcpiTables/ には FADT / FACS / MADT / GTDT / DBG2 と DSDT (CPU, GIC, timer, UARTB(NVDA0100), SDMMC1/4) がある。
MCFG / PPTT / PCIe・XHCI の DSDT デバイスは無い。実機未検証。

## Lumia950XL から得られた点
1. FADT: HW_REDUCED_ACPI | LOW_POWER_S0_IDLE_CAPABLE, ArmBootArch = PSCI_COMPLIANT。Lumia は ResetReg を持つ。winx は NULL_GAS (再起動 = PSCI SYSTEM_RESET に依存。Switch の PSCI が使えるか要確認)。
2. DSDT のコンパイラ: iasl 新版は Windows が解釈できない ACPI 6.x opcode を出す。Lumia は MS asl.exe (WDK 17134) でコンパイルした AML を同梱。winx は build で iasl を使っている → Windows 側で DSDT が読めない可能性。対策: iasl 旧版(20190108)で試す or AML を asl.exe で作って同梱。
3. MCFG: 単一 ECAM 領域 (Base, Segment, StartBus, EndBus)。Tegra X1 の PCIe は標準 ECAM ではない (AFI の FPCI BAR0 経由で 4KB 単位アクセス) → MCFG だけでは Windows の標準 PCIe ドライバは動かない。Wi-Fi(BCM4356)をWindowsに見せるには ECAM 互換のウィンドウを作る(ファームで AFI 設定し、4 KiB×256 の連続領域に見せるのは困難) か、諦めてUSB経由にする。
4. PPTT: Windows on Arm では CPU トポロジ用に推奨。Lumia は PPTT.aslc を同梱。
5. PEP: Lumia は Qualcomm 専用 PEP (QCOM...)。Tegra には存在しないため無し。PEP 無しでも起動自体は可能と思われるが電源管理は効かない(未確認)。
6. ブート: Lumia では EFIESP に UEFI.elf + BCD エントリを作る。Switch では hekate が bl33.bin を直接ロードするので不要。BCD (testsigning / nointegritychecks) はディスク側の設定として別途。

## 優先順位案
A. DSDT を Windows が読める AML にする (iasl 旧版/asl.exe の検証)
B. PPTT と MCFG(必要な場合のみ) の追加
C. XHCI (NVDA/標準 PNP0D10 風) を DSDT に追加し、XUSB の初期化を UEFI 側に実装済みの前提で Windows に見せる
D. Wi-Fi は ECAM 問題があるため後回し (USB Wi-Fi ドングルの方が現実的)

## 未確認
上記はリポジトリ内のコードとテーブルを読んだ範囲での判断。Windows が実際に何を要求するかは実機/ VM での確認が必要。

## 実施結果 (2026-10-15)
- B: PPTT 追加 (AcpiTables/Pptt.aslc, パッケージ1 + コア4, キャッシュ無し)。ビルド rc=0、生成された Pptt.acpi を目視確認 (長さ 0x88=36+20*5、コア flags 0x0A、親 offset 0x24、ID 0-3)。
- A: 現行 DSDT (iasl 20230628 でコンパイル, 610 バイト, revision 1) の AML を逆アセンブルして確認。使われている opcode は従来型のみ (Name/Method/Store/Or/Increment/CreateXField/ConcatenateResTemplate 等)。Lumia で問題になった ACPI 6.x 系の新 opcode は見当たらない。
  → 現状の DSDT では asl.exe は不要と判断。ただし「Windows がこの AML を実際に読めるか」は未確認。DSDT を大きくする (XHCI 等) ときは同じ確認を繰り返すこと。
- 参考: Dsdt.asl の `CHN2 = Arg1++` は AML では Store(Increment(Arg1), CHN2) で、意図通り Arg1+1 になる (変更せず)。
- C: DSDT に XHCI デバイス `XHC0` を追加 (`-D SWITCH_ACPI_XHCI=1` のときだけ。既定 OFF)。レジスタ 0x70090000/0x8000, SPI 39 → INTID 0x47 は tegra210.dtsi と一致。ビルド rc=0、AML を逆アセンブルして GCRS(0x70090000, 0x8000, 0x47) を確認。EMMC フラグとの併用もビルド確認済み (DSDT の ASLPP フラグを DEFINE で合成)。XUSB ファーム未ロード/メールボックス未処理だと Windows からは死んだコントローラに見える点に注意。Tegra の XHCI は FPCI/IPFS 側の初期化とメールボックス応答が要るため、Windows の標準 usbxhci で動くかは未確認 (最大の不確定要素)。

## EhciDxeTegra 監査 (2026-10-16)
EhciDxeTegra は upstream EhciDxe とほぼ同一 (差分は EhciReg.c の「リセット後にホストモードへ戻す」のみ)。Tegra210 (ChipIdea, 内蔵 TT, コンパニオン無し) 固有の不足を調べた:
- 修正: EhcGetRootHubPortStatus が「有効なら高速」と決め打ちしていたため、ルートポートのフルスピード機器が高速扱いになり転送が全滅する恐れがあった。HOSTPC1_DEVLC.PSPD (op+0x84, bits 26:25: 0=FS, 1=LS, 2=HS) から速度を取得するよう変更 (Linux ehci_port_speed と同じ解釈)。
- 確認のみ (変更なし): DMA は NonDiscoverablePciDeviceDxe の PciIo 経由でキャッシュ処理される。ExitBootServices では EhcResetHC (+ ホストモード復帰) が呼ばれる。
- 未対応 / 未確認: ポートサスペンド/PHY 低電力 (PHCD)、USB-C 抜き差し時の PD/bq24193 状態変化との連動、高速以外の機器の TT 経由転送の実機確認。GBAtemp OP の「接続・電源状態の変化で USB が影響を受ける」はこれらに該当する可能性があるが、再現ログが無いので原因は未特定。
