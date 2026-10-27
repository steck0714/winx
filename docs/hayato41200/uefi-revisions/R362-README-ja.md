# R362 — XUSBのFull-Speedのクロック（PLLU 48MHz / PLLU_OUT1 / CLK_SOURCE_XUSB_FS）を設定する（R361ベース）

## 目的

R361で、VREG_LEVとfuse較正値を設定した。その結果、LIGHTSPEEDのレシーバーとキーボードがつながった（LEDが点灯）。しかし、UEFIのUsbBusでは、まだ列挙できていない（`P5=00000603`、`UsbIo=0`）。`../Switch-UEFI-XUSB-R361-FS-VREG-FUSE-CALIB/R361-RESULT-ja.md` を参照。

UEFIの起動処理（R243）では、XUSBのクロックの源として、host（CAR+0x600）とSS（CAR+0x610）しか設定していない。FSのロジックのクロック（`CLK_SOURCE_XUSB_FS`）は未設定。Hekate（`_xusbd_init_device_clocks`）とLinux（xusbのノードの `xusb_fs_src`）に合わせて、これを設定する。

**注意（R361の結果からの推測）:** レシーバーが目覚めた（SOFが届いている）ので、FSのクロックが列挙の失敗の原因である可能性は下がっている。R362は「念のための確認」と「クロックの値を記録すること」を兼ねる。

## 変更点（`UsbPowerAudit-R362.c` の `R362ConfigureFsClocks`）

接続処理（`R362ProductionAttach`）の最初、PADCTLのホスト設定の前に実行する。どの値も**違うときだけ**書き込み、前後の値を表示する。

| レジスタ | 書き込み |
|---|---|
| CAR+0x0C0 `PLLU_BASE` | bit25（48MHz出力）が0なら1にする |
| CAR+0x0C4 `PLLU_OUTA` | bit0（PLLU_OUT1のリセット解除）が0なら1にする |
| CAR+0x608 `CLK_SOURCE_XUSB_FS` | bit[31:29] を2（FO 48MHz）にする（Hekateと同じ） |

- `CLK_SOURCE_XUSB_FALCON`（0x604）と `XUSB_SS`（0x610）は表示するだけ。
- 表示: `R362 CLK before: PLLU_BASE=... (EN LOCK USB480 USB60 USB48) PLLU_OUTA=... FALCON_SRC=... FS_SRC=... SS_SRC=...` と `R362 CLK after: ... PASS/FAIL`。
- 読み戻しが合わなかったときは `R362 WARNING` を表示して先へ進む（HSには関係しない）。

変えていないもの: R361のVREG_LEVとfuse較正値、PADCTLのホスト設定、IPFS/FPCIの設定、XhciDxe、DSDT、ROM内のファームウェア、45秒のブート待ち。

## 試験の手順（UEFI）

1. OTG端子にLIGHTSPEEDのレシーバーだけを挿し、R362で起動する（SDは無くてもよい）。
2. `R362 CLK before/after`、`R362 FUSE:`、`R362 FS PHY after`、`R362 USB STATUS #1〜#4` の行を撮影する（起動直後から動画で撮るのが確実）。
3. キーボードのLEDが点灯しているかを見る。

## 読み方

| 結果 | 意味 |
|---|---|
| `CLK before` で、FS_SRCのsrcが2、USB48が1、PLLU_OUTAのbit0が1 | もともと設定済みだった。FSのクロックは原因ではない（書き込みも起きていない） |
| `UsbIo[..]: VID:PID=046D:xxxx` が出る | FSのクロックが原因だった |
| `UsbIo=0` のまま | 残りはEDK2側の可能性が高い（UsbBus / XhciDxe のFSの列挙）。Windowsでの結果と比べる |

## 成果物と検証

- ROM: `D:\Codex\coreboot-R362.rom`（10,485,760 bytes）
- SHA-256: `fc80c49636a9f9a888612f9b2a3d00bc5ffb7cdbde1ae640b2c20cf61b795095`
- ビルド: `package-R362.sh`（WSL Ubuntu-24.04）。`- Done -` まで完了し、新しいコンパイル警告は無し。
- 検証済み:
  - UsbPowerAuditのバイナリに `R362 CLK before/after` と、R361までの各表示文字列がある
  - XhciDxeはR361と同一
  - 作業ツリーのDSC/FDFは元に戻り、`Firmware/` は削除されている。`EDK_SOURCE_RESTORE_MATCH=YES`
  - 出力フォルダーのROMとコピー先のSHA-256が一致
- 差分: `R361-to-R362-UsbPowerAudit-semantic.diff`
- **未確認:** 実機での動作。CARのビット位置（PLLU_BASEのbit25 = 48MHz出力、PLLU_OUTAのbit0）は、Hekateと既存のコード（R2xxのPLLUの表示）の定義に基づく。
