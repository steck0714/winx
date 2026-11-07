# R364 — UEFI で CPU を 691 MHz から約 1020 MHz に上げる（R363 ベース）

## 背景

- **[事実] 今の CPU は 691.2 MHz・950 mV。** coreboot の `CONFIG_PLLX_KHZ=700000` から、PLLX = 38.4 MHz × 18 = **691.2 MHz** になっている（M=1、N=18）。Windows の棚卸しでも `CurrentClockSpeed=691` だった（SMBIOS の値は 1900 なので、Windows が実際に測った値）。VDD_CPU は、coreboot の `pmic.c` で MAX77621（I2C5 の 0x1B）の VOUT=0xB7、つまり **950 mV**。
- **[事実] この個体は CPU Speedo ID 3。** L4T の dmesg に `CPU Speedo ID 3 ... SKU: 0x83 CPU Process: 0` とある。Linux の `TEGRA210_CPU_CVB_TABLE_ODN`（DFLL 用、speedo に依存しない値、最小 825 mV・最大 1227 mV）によると、必要な電圧は次のとおり。

| 周波数 | 必要な電圧 |
|---|---|
| 714 MHz | 886 mV |
| 1020 MHz | 985 mV |
| 1428 MHz | 1117 mV |
| 1785 MHz | 1228 mV |

- **[判断] 電圧は表の値より上げておく。** PLLX は開ループで、DFLL のような電圧の自動調整が無い。今の 691 MHz・950 mV も、表の値より約 60 mV 高い。そこで R364 は、1020 MHz の 985 mV に余裕を足して **1025 mV** にする。

## 変更点（`UsbPowerAudit-R364.c` の `R364RaiseCpuClock`）

USB の処理の前（I2C を初期化した直後）に、次の順で行う。

1. **前の状態を読んで表示する。** PLLX_BASE と MISC、CCLK_BURST_POLICY、SUPER_CCLK_DIVIDER、MAX77621 の VOUT と VOUT_DVC。想定と違う状態のときは、何もせずに **SKIP** する。想定は次の3つ。
   - PLLX が有効、ロック済み、バイパスしていない
   - CCLK が PLLX（0x20008888）
   - VOUT が有効
2. **電圧を先に上げる。** VOUT_DVC と VOUT に 0xC3（1025 mV）を書き、2 ms 待って読み戻す。合わなければ元の値に戻して終わる。
3. **クロックを切り替える。**
   1. CCLK を一時的に PLLP（408 MHz、0x20004444）に切り替える。
   2. PLLX を止めて **M=2、N=53**（38.4 / 2 × 53 = **1017.6 MHz**）に書き換え、もう一度有効にする。
   3. ロックするのを最大 10 ms 待つ。
   4. ロックしたら、CCLK を PLLX に戻す。**ロックしなければ、元の PLLX・CCLK・電圧に戻す。**
4. **実際に速くなったかを測る。** ループの速さを、ARM の汎用タイマー（`cntvct_el0`）で前後に測る。比が 1.47 前後なら、想定どおり速くなっている。
5. **温度を表示する（新規）。** TMP451（I2C1 の 0x4C）から、SoC とボードの温度を読む。表示は `R364 ... temp: SoC=.. C board=.. C`。起動直後と、`USB STATUS` のたびに出る。

ほかは R363 と同じ（USB、ハブでの充電）。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R364.rom`（10,485,760 bytes）
- SHA-256: `b7e47d0372b477d5b6c3b5b839bba24b4ab2346f3d0d8b14a9bc9338a24cf524`
- ビルド: 完了。警告は R363 と同じもの（タイムスタンプの違いだけ）。`EDK_SOURCE_RESTORE_MATCH=YES`。
- R364 の表示文字列が `UsbPowerAudit-R364.efi` に入っていることを確認済み。
- 差分: `R363-to-R364-UsbPowerAudit.diff`
- **未確認:** 実機での動作。特に次の3点。
  - PLLX がロックするか
  - 1025 mV で安定するか
  - 温度

## 試験の手順

1. いつもの構成（ハブと充電器、または OTG）で、R364 で起動する。最初は SD 無しで、UEFI の画面だけ見るのがよい。
2. 次の行を撮影する。
   - `R364 CPU before`
   - `VDD_CPU before`
   - `step 1`、`step 2`
   - `CPU after`
   - `loop rate ... ratio=... => about ... MHz`
   - `temp: SoC=..`
3. うまくいったら、SD を入れて Windows で次を確認する。
   - タスクマネージャーの CPU の速度が、約 1.02 GHz になっているか
   - 画面の動きが軽くなったか
   - しばらく使ったときの安定性と熱

## 読み方

| 表示 | 意味 |
|---|---|
| `R364 SKIP: ...` | 想定外の状態だったので、何もしていない（691 MHz のまま）。前の行の値を確認する |
| `step 1 ... FAIL` | 電圧を書けなかった。元の電圧に戻している |
| `step 2 ... FAIL (no lock)` | PLLX がロックしなかった。元の PLLX と電圧に戻している |
| `ratio=1.4x` で `about 10xx MHz` | 成功 |
| 起動途中で固まる、または再起動する | 電圧が足りない可能性がある。R363 に戻して報告する（次の版で 1050 mV などにする） |

## 注意

- **ファンの制御はまだ無い。** 1 GHz 前後は Horizon の標準のクロックだが、Windows で重い処理を長く続けると熱がこもる可能性がある。`temp: SoC=` の値を見ながら試すこと（目安として 70℃ を超え続けるなら止める）。
- **これより上のクロック（1428 MHz や 1785 MHz、L4T のようなオーバークロック）は、温度とファンの制御を入れてから**、段階的に追加する。
