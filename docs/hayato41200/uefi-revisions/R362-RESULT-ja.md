# R362 実機結果（ユーザー報告、2026-10-24）

構成: R362 + Windows入りのSD + LogitechのLIGHTSPEEDレシーバー（キーボードとマウス）

## 結果

**[事実]（ユーザー報告）** **Windows 10 ARMを、USBキーボードで操作できるようになった。マウスも使える。**

UEFIの画面（`R362 CLK before/after`、`R362 FUSE`、`R362 USB STATUS`）は撮影していない。

## 解釈

**[事実]** Switch上のWindows 10 ARM（build 19044.1288）で、次のすべてが動いた。
- 標準の USBXHCI → USBHUB3
- Full-Speedのレシーバー → HID（キーボード、マウス）

HSのUSBメモリ（USBSTOR）は、R360で認識を確認済み。

**[未確認]**
- **R361とR362のどちらの変更が効いたか:** FSで動いた理由が、R361の変更（VREG_LEVとfuse較正値）だけなのか、R362の変更（FSのクロックの設定）も必要だったのかは、分かっていない。R362の `CLK before` の値を撮っていないため、FSのクロックが元から設定済みだったか（書き込みが起きたか）も不明。
- **UEFIでFSの機器が列挙されたか:** R362のUEFIでの `UsbIo` の表示は、撮っていない。

## ここまでの必要条件のまとめ（UEFIがWindowsへ引き渡す状態）

1. **XUSBの起動:** 電源・クロック・リセット（R243）、ROM内のファームウェアとFalcon（R246/R249、R359）。
2. **VBUSとUSB-Cの設定:** BM92Tのホスト検出とBQ2419xのOTG（VBUS）、BQのwatchdogの無効化。
3. **PADCTLのホスト設定:**
   - パッド0とbias padの電源（PDビットのクリア）
   - `VBUS_ID` をID override、ID=GND
   - **`PAD_MUX[1:0]=XUSB`**
   - USB2_TRK、bias=XUSB、`PORT_CAP=OTG`
4. **FS/LSのPHY:** `BATTERY_CHRG_OTGPAD0_CTL1` のVREG_LEV=1と、fuse較正値（HS_CURR_LEVEL / TERM_RANGE_ADJ / RPD_CTRL）（R361）。
5. **IPFSとFPCI:** `INTR_MASK` のIP_INT_MASK、CLKGATE_HYSTERESIS=0x80、FPCIのIO/MEM/BUS_MASTER（R360）。**Windowsで割り込みを受け取るために必須。**
6. （R362）PLLUの48MHz出力、PLLU_OUT1、`CLK_SOURCE_XUSB_FS`=FO 48MHz。必要かどうかは未確認。
7. **UEFIのUSBスタック:** XhciDxe（`UsbHcMem` のDMA確保をWCにする修正）、UsbBus、UsbMassStorage。ExitBootServicesではRS=0にするだけで、他の状態は引き継ぐ。
8. **ACPI:** DSDTの `XHC0`（PNP0D10）。Windows標準のUSBXHCIがbindする。
