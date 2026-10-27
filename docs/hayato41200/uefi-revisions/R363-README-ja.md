# R363 — USB-C ハブ＋充電器で、充電しながら USB ホストとして使う（R362 ベース）

## 目的

R362 までは、相手が「受電側」（OTG でつないだ USB 機器やレシーバー）のときだけ XUSB を起動していた。給電側（充電器をつないだ USB-C ハブ）では、30秒の待ちがタイムアウトして、USB の起動自体を飛ばしていた。R363 では、給電側のときに **Switch を充電モードにしたうえで、USB ホストとして XUSB を起動する**。

想定する構成: ELECOM DST-C30SV（USB-C ハブ）＋ Anker Nano Charging Station（最大100W）。

## 参考にしたもの

- **L4T の BM92T ドライバー** `drivers/misc/bm92txx.c`（CTCaer）: レジスタとビットの定義、DR_SWAP コマンド（0x1818）、充電電流の上限の表（5V/9V: 2000mA、12V: 1500mA、15V: 1200mA）。
- **L4T の実機ログ**（`Claude-Handoff-20261023\Linux-reference\dmesg.txt`）: PD 充電器をつないだとき、`Status1=0x4D80`（受電、UFP）で契約を結び、「Device in UFP and DRD not supported!」と表示されたうえで、USB ホストとして動いていた。
- **Hekate のバッテリー情報**: 標準の設定のまま、BM92T が自分で 15V の契約を結んでいた。そのため R363 では、自分から RDO（電圧の要求）は送らない。

## 変更点（`UsbPowerAudit-R363.c`）

1. **相手の判定**（最大30秒、3回連続で同じ判定になったら確定）
   - **ホスト**（従来どおり）: `INSERT + SRC_MODE + OTG_INSERT`。Switch が VBUS を出す（BQ を OTG モードにする）。
   - **受電**（新規）: `INSERT + VSAFE` かつ `SRC_MODE=0` かつ、データの役割が DFP ではない。
   - 表示: `t=..s S1=.. S2=.. DP=.. INS SRC OTG VSAFE SNK DR`
2. **受電のとき**（`R363ConfigureSink`）。**OTG（VBUS の出力）は使わない。**
   1. **BQ REG01 を充電モードにする**（CHG_CONFIG=01）。最初に行うのは、前の R362 のセッションの OTG が残っている可能性があるため（watchdog を止めているので、OTG のまま残る）。
   2. **給電側が出せる電圧と電流（PDO）を表示する**（BM92T 0x08）。
   3. **BM92T が結んだ契約を読む**（最大5秒。`SPDSNK` と `CURRENT_PDO` 0x28、`CURRENT_RDO` 0x2B）。
   4. **必要ならデータの役割を交換する。** 給電側が DRD に対応していて、Switch が UFP のときだけ `DR_SWAP` を送る（L4T と同じ）。失敗しても、ホストとして先へ進む。
   5. **BQ REG00 を設定する。** HIZ を解除し、入力電流の上限を契約の電圧に合わせる（L4T の表）。契約が無いときは 5V・500mA とする。
   6. **BQ の watchdog を止める**（R362 と同じ。Windows に入ってからも設定が戻らないように）。
   7. **充電の状態を表示する**（下記）。
3. その後は R362 と同じ。R295 の経路の設定 → ファームウェアと Falcon → R362 のクロックと PADCTL のホスト設定 → IPFS/FPCI → XhciDxe/UsbBus。
4. **状態の表示**（`R362 USB STATUS #1〜#4` のたびに、合わせて出る）
   - `R363 status(sink) BQ: fast-charge vbus=adapter REG00=.. REG01=.. SYS=.. FAULT=..`
   - `R363 status(sink) battery: ..% ....mV current=.. mA (+ = charging) BM92T S1=....`
   - 電流は、残量計 MAX17050 の Current レジスタを、検出抵抗 5mΩ として換算した値（生の値も表示する）。

変えていないもの: XhciDxe（R362 と同一）、DSDT、ROM 内のファームウェア、45秒のブート待ち、OTG の経路。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R363.rom`（10,485,760 bytes）
- SHA-256: `21bdf4a255d02ab44379ad4c5ff5675357a1a3eb99629b328b0d9ce9e7caead1`
- ビルド: `package-R363.sh`（WSL Ubuntu-24.04）。完了し、新しいコンパイル警告は無し。`EDK_SOURCE_RESTORE_MATCH=YES`。
- 確認済み:
  - `UsbPowerAudit-R363.efi` に、R363 の表示文字列が入っている
  - Xhci、XhciSched、UsbHcMem は R362 と同一
- 差分: `R362-to-R363-UsbPowerAudit.diff`
- **未確認:** 実機での動作のすべて。特に次の4点。
  - DST-C30SV がデータの役割の交換なしで USB をつなぐか
  - BM92T の契約の電圧（15V を想定）
  - バッテリーの電流が実際にプラスになるか
  - MAX17050 の電流の換算（5mΩ）が合っているか

## 試験の手順

1. **事前に:** Anker の充電器と DST-C30SV を先につなぎ、ハブにレシーバー（または USB メモリ）を挿しておく。
2. **起動する:** ハブを Switch の USB-C に挿してから、R363 で起動する。最初は、SD 無しで UEFI の画面だけ見るのがよい。
3. **撮影する:** 起動直後から動画で撮るのが確実。
   - `t=..s ... VSAFE=1 SNK=.. DR=..` の行
   - `R363 BQ REG01 ...`
   - `R363 source PDOs` と、各 PDO
   - `contract PDO`
   - `data role` と `DR_SWAP`
   - `R363 BQ REG00 ...`
   - `after charge setup` の2行
   - `R362 USB STATUS #1〜#4` と、その後の `R363 status(sink)` の行
4. **確認する:** レシーバーの LED が点灯するか、`UsbIo` に機器が出るか。
5. **Windows で試す:** うまくいったら、SD を入れて Windows で、キーボード・マウスの操作と充電（使いながら残量が減らないか）を見る。

## 読み方

| 表示 | 意味 |
|---|---|
| `SAFE TIMEOUT: host/sink gate absent` | 受電の判定ができなかった。`t=` の行の S1 を見て、判定の条件を見直す |
| `contract PDO 0: 15000mV ...` と `input limit 1200 mA` | 想定どおり、15V で受電している |
| `BQ: fast-charge vbus=adapter` | 充電中 |
| `battery: current=+数百 mA` | 使いながらでも充電されている |
| `battery: current=-...` | 消費が充電を上回っている（次の版で、9V などの別の契約や上限の見直しを検討する） |
| `UsbIo=0` のままで、P5 が接続なし | ハブの USB がつながっていない。`DR_SWAP` の結果と `data role` を確認する |

## 注意

- **充電器は、ハブを Switch に挿す前につないでおくこと。** 起動した後に充電器を抜くと、ハブの電源が切れて、USB が止まる（R363 は、起動時の状態に合わせて一度だけ設定する）。
- **給電側でつないだ後は、Switch は VBUS を出していない。** OTG で USB 機器を直接挿す場合は、今までどおりホストの経路になる。
