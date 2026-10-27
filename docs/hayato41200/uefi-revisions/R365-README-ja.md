# R365 — CPU を約 1020 MHz に上げる（R364 の修正版）

R364 は、PLLX を設定し直す間、CPU を一時的に PLLP で動かそうとして固まった（`../Switch-UEFI-XUSB-R364-CPU-1020MHZ/R364-RESULT-ja.md`）。原因は、T210 で PLLP から CPU への分岐（`CLK_ENB_Y` の bit31）が無効のままだったこと。

## R364 からの変更点

- **一時的なクロック源を変えた。** CCLK の一時的な行き先を **CLK_M**（38.4 MHz、`CCLK_BURST_POLICY=0x20000000`）にした。CLK_M はゲートが無いので、常に使える。この間（10〜数十 µs）だけ、CPU は 38.4 MHz で動く。
- **確認用の表示を追加した。** `R365 CCLKG_POLICY(0x368)=.. CLK_OUT_ENB_Y=.. (bit31 PLLP_OUT_CPU=..)`。R364 の原因を確かめるためのもの。

ほかは R364 と同じ。
- 電圧を先に 950 → 1025 mV に上げる
- PLLX を M=2、N=53 にする（1017.6 MHz）
- ロックしなければ、元の PLLX と電圧に戻す
- ループの速さを前後で測る
- TMP451 の温度を表示する

## 成果物と検証

- ROM: `D:\Codex\coreboot-R365.rom`（10,485,760 bytes）
- SHA-256: `850b5accc3cb6a1183496320c292aab7e0000c75e5898607a7f1397a3f570b19`
- ビルド: 完了。新しい警告は無し。`EDK_SOURCE_RESTORE_MATCH=YES`。
- 差分: `R364-to-R365-UsbPowerAudit.diff`
- **未確認:** 実機での動作。

## 試験の手順と読み方

R364 の README と同じ。撮影してほしいのは次の行。
- `R364 CPU before`
- `R365 CCLKG_POLICY ... PLLP_OUT_CPU=`（0 なら、R364 の原因の説明が正しい）
- `step 1`、`R365 step 2`
- `CPU after`
- `loop rate ... => about ... MHz`
- `temp: SoC=`
