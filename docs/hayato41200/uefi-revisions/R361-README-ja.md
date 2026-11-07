# R361 — Full-Speed用のPHY設定（VREG_LEVとfuse較正値）を加える（R360ベース）

## 目的

R360で、HSのUSBメモリはUEFIでもWindowsでも動いた（`../Windows-USB-Test/R360-Windows-HS-result-ja.md`）。一方、Full-Speedの機器（LogitechのLIGHTSPEEDレシーバー）は、UEFIでも次の状態のままだった（`../Windows-USB-Test/R360-receiver-observation-ja.md`）。

- P5は `00000603`（FS、CCS=1、PED=1、U0）
- `UsbIo handles=0`（列挙できない）
- レシーバーの消費は約1.4mA（サスペンドの水準）

FSのデータ信号がレシーバーに届いていないと考えられる。原因の候補は、PADCTLのFS/LS駆動の設定（`USB2_BATTERY_CHRG_OTGPAD0_CTL1` のVREG_LEV）が未設定（0）であることと、fuse由来の較正値が未設定であること。R361は、この2つをHekate/Linuxと同じに設定する。

## 変更点（`UsbPowerAudit-R361.c` の `R361ApplyFsPhyCalibration`）

`R361ApplyHostPadctl` の最初、パッドの電源を入れる前に実行する（Hekate `_xusb_init_phy()` と同じ順序）。

| レジスタ | フィールド | 設定値 |
|---|---|---|
| `OTG_PAD0_CTL0`（0x088） | [5:0] HS_CURR_LEVEL | `FUSE_USB_CALIB[5:0]` |
| `OTG_PAD0_CTL1`（0x08C） | [6:3] TERM_RANGE_ADJ | `FUSE_USB_CALIB[10:7]` |
|  | [30:26] RPD_CTRL | `FUSE_USB_CALIB_EXT[4:0]` |
| `BATTERY_CHRG_OTGPAD0_CTL1`（0x084） | [8:6] | `010b`（VREG_LEV=1。Hekateの `(CTL1 & 0xFFFFFE3F) | 0x80`） |

**fuseの読み方:**
- 場所: `FUSE_USB_CALIB` = 0x7000F800+0x1F0、`FUSE_USB_CALIB_EXT` = +0x350。Hekateの定義で、Linuxの SKU_CALIB_0 / USB_CALIB_EXT_0 に当たる。
- 安全のため、FUSEのクロック（CAR `CLK_OUT_ENB_H` bit7、Linuxの `TEGRA210_CLK_FUSE=39`）が有効なときだけ読む。クロックが止まっているブロックを読むと、固まるおそれがあるため。
- クロックが無効、または読んだ値が0やすべて1だったときは、このSwitchのLinux dmesgの値（`TEGRA_FUSE_SKU_CALIB_0 = 0x09248c11`、`TEGRA_FUSE_USB_CALIB_EXT_0 = 0x4`）を使う。どちらを使ったかを `source=` に表示する。
- Linuxの値と一致したかを `MATCH` / `DIFF` で表示する。

**表示:**
- `R361 FUSE: CLK_OUT_ENB_H=... source=... USB_CALIB=... USB_CALIB_EXT=... MATCH/DIFF`
- `R361 FS PHY before/after: BC1 OTG0 OTG1 (HS_CURR TERM RPD VREG[8:6]) PASS/FAIL`

**この端末で予想される値:** Linuxの値から計算すると、HS_CURR_LEVEL=17（今は32）、TERM_RANGE_ADJ=8（今も8）、RPD_CTRL=4（今は0）、VREG[8:6]=2（今は0）になる。

**読み戻しが合わなかったとき:** `R361 WARNING` を表示して先へ進む（パッドの電源、ID、MUXの設定はR360と同じ）。

変えていないもの: XhciDxe、DSDT、ROM内のファームウェア、IPFS/FPCIの設定（R360）、45秒のブート待ち。いずれもR360と同一。

## 試験の手順（UEFIだけで判定できる）

1. OTG端子にLIGHTSPEEDのレシーバーだけを挿し、R361で起動する（SDは無くてもよい）。
2. 撮影するもの:
   - `R361 FUSE: ...` と `R361 FS PHY after: ... PASS` の行（画面が流れるので、起動直後から動画で撮るのが確実）
   - `R361 USB STATUS #1〜#4`
3. あわせて、キーボードのLIGHTSPEEDのLEDが点灯に変わるか、USBテスターの電流が増えるか（1.4mAより大きくなるか）を見る。

## 読み方

| 結果 | 意味 |
|---|---|
| `UsbIo[..]: VID:PID=046D:xxxx`（Logitech）が出て、LEDが点灯し、電流が増える | FSのPHY設定が原因だった。次はWindowsでキーボードを試す |
| `UsbIo handles=0` のまま、LEDが点滅したまま | VREG_LEVと較正値だけでは足りない。次は bias tracking（BIAS_PAD_CTL1）やUTMIPLLの設定を調べる |
| `FS PHY after: ... FAIL` | 書き込みが反映されていない。値を共有してもらう |
| HSのUSBメモリが動かなくなった | HS_CURR_LEVELなどの較正値が逆効果だった（R360に戻して切り分ける） |

## 成果物と検証

- ROM: `D:\Codex\coreboot-R361.rom`（10,485,760 bytes）
- SHA-256: `cd14b71d574affed28971009223207036101c9614dad7236561fc0abf3cba7fd`
- ビルド: `package-R361.sh`（WSL Ubuntu-24.04）。`- Done -` まで完了し、新しいコンパイル警告は無し。
- 検証済み:
  - UsbPowerAuditのバイナリに `R361 FUSE` / `R361 FS PHY after` / `R361 IPFS/FPCI after` / `R361 embedded XUSB firmware: PASS` の文字列がある
  - XhciDxeはR360と同一
  - 作業ツリーのDSC/FDFは元に戻り、`Firmware/` は削除されている。`EDK_SOURCE_RESTORE_MATCH=YES`
  - 出力フォルダーのROMとコピー先のSHA-256が一致
- 差分: `R360-to-R361-UsbPowerAudit-semantic.diff`
- **未確認:** 実機での動作。fuseのアドレスと、FUSEのクロックのビット位置は、Hekate/Linuxの定義と、Linuxで読んだ値に基づく推定。実機でのfuseの値は、`R361 FUSE` の行で確かめる。
