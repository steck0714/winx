# R361 実機結果（写真2枚、LIGHTSPEEDのレシーバー、2026-10-24）

## 画面

- `... RPD=4 VREG[8:6]=2) PASS`（`R361 FS PHY after` の行の末尾）
- `R361 PADCTL after: MUX=00040055 CAP=00000003 BC0=000C0000 OTG0=00CD1011 OTG1=12100040 BIAS0=260E0010 VBUS_ID=00011400`
  - OTG0 = `00CD1011` → HS_CURR_LEVEL=0x11（17）。fuse/Linuxの値どおり。
  - OTG1 = `12100040` → RPD_CTRL（bit30:26）=4、TERM_RANGE_ADJ=8。
- `R361 IPFS/FPCI after: INTR_MASK=00010000 ... FPCI_CMD=0007 ... PASS`
- `R361 ConnectController status=Success USB2_HC=INSTALLED`
- `R361 USB STATUS #1〜#3: USBCMD=00000001 USBSTS=00000010 P5=00000603`、`UsbIo handles=0`、`BlockIo handles=0`

## 解釈

**[事実]**
- **設定は反映された:** VREG_LEVとfuse較正値（HS_CURR_LEVEL、TERM_RANGE_ADJ、RPD_CTRL）は、書いたとおりに反映された（PASS）。
- **列挙は変わらない:** Full-Speedのレシーバーは、相変わらず接続と有効化（`P5=00000603`）までで、列挙はできない（UsbIo=0）。R360と変わらない。

**[未確認]**
- `R361 FUSE:` の行（fuseを読んだか、Linuxの値を使ったか）。写真に写っていない。
- キーボードのLEDと、USBテスターの電流。
- HSのUSBメモリへの影響。

**[推測]** 次の候補は、XUSBの**Full-Speedのロジックのクロック**。
- **R243での設定:** CARの `CLK_SOURCE_XUSB_CORE_HOST`（0x600）と `CLK_SOURCE_XUSB_SS`（0x610）しか設定していない。
- **Linuxの使い方:** LinuxのDTのxusbノードは、`xusb_host`、`xusb_falcon_src`、`xusb_ss`、`xusb_ss_src`、`xusb_hs_src`、**`xusb_fs_src`**、`pll_u_480m`、`clk_m`、`pll_e` を使っている。
- **Hekateの使い方:** Hekate（device側）は、`PLLU_OUTA |= 1`（PLLU_OUT1のリセットを解除）と、`CLK_SOURCE_XUSB_FS = (… & 0x1FFFFFFF) | (2 << 29)`（FO 48MHz）を設定している。
- **症状との関係:** HSは480MHz系のクロックで動くので、FSのクロック（48MHz）が無くてもHSは動く。一方、FS/LSのSOFやパケットの処理には、FSのロジックのクロックが必要と考えられる。「HSは動き、FSだけ接続・有効化の後に通信できない」という症状と合う。

## 次の案（R362）

接続の前に、次を行う。
- **値の表示:** CARの PLLU_BASE（0x0C0。bit22 USB480、bit23 USB60、bit25 USB48）、PLLU_OUTA（0x0C4）、`CLK_SOURCE_XUSB_FALCON`（0x604）、`XUSB_FS`（0x608）、`XUSB_SS`（0x610）を表示する。
- **設定:** Hekate/Linuxに合わせて、PLLUの48MHz出力、PLLU_OUT1のリセット解除、XUSB_FSのソース（2 = FO 48MHz）を設定し、読み戻す。

## 追記（ユーザー報告、2026-10-24）

**[事実]** R361では、キーボードのLIGHTSPEEDのLEDが**点灯に変わった**（キーボードとレシーバーがつながった）。R360までは、UEFIの段階から点滅していた。

**[推測]**
- **FSの信号は直った:** VREG_LEVとfuse較正値の設定で、Full-Speedの信号（SOFなど）がレシーバーに届くようになった。レシーバーはサスペンドから抜け、無線が動いている。
- **残っているのは列挙の最終段階:** UEFIで `UsbIo handles=0` のままなので、UsbBusでの列挙（Address Device / ディスクリプタの取得 / SET_CONFIGURATION）のどこかで、まだ失敗している。SOFが流れているので、FSのロジックのクロックが原因である可能性は下がった。EDK2の処理（FSのMPS0の扱いなど）が原因の可能性もある。
- **Windowsでは動く可能性がある:** WindowsのUSBXHCIは独自に列挙するので、R361でキーボードが動く可能性がある。
