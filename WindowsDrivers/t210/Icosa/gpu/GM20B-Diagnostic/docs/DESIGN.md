# winx-gm20b 設計規約（全ステージ実装者が従う契約）

目的: Nintendo Switch (Tegra X1 / GM20B) の GPU を、Windows 11 ARM64 から段階的に立ち上げるドライバを、
**実機検証前の「理論値」**（NVIDIA 純正 nvgpu・nouveau の一次資料）から先に作る。
実機で測れるのは週1回なので、毎回の検証から最大の情報を得て、1段ずつ前進できる作りにする。

## 0. 前提（bl33.bin の読み取り専用解析で確認済み）

- bl33 の DSDT（2185 B）には CPU/UART/SD/XHCI/GPIO/I2C×4/BAT/FAN/BT/TCH/AUD/PWRB/CPUC/RTC があり、**GPU ノードは無い**。
  → ドライバは ACPI を前提にしない。**レガシー WDM サービス（PnP 無し）＋固定物理アドレス**で読み込む。bl33 は変更不要。
- `CPUC`（`SWOA0701`）が **CAR 0x60006000+0x1000 を占有**している。GPU ドライバは CAR を共有する立場。
  → CAR は **SET/CLR エイリアスレジスタのみ**で操作する。読み-修正-書きで共有 PLL 等を壊さない。
- `I2C5`（`SWOA0201` @0x7000d000）が存在（PMIC=MAX77621 が載るバス）。ただしこのドライバは**既定では PMIC/電圧に触れない**。
- 割り込みは使わない（全てポーリング）。GPU の GIC SPI は 157/158（GSIV 189/190）。
- 実機で 4 GiB DRAM は 0x80000000〜。GPU は CPU 物理アドレスを直接使う前提（SMMU バイパス）。
  ステージ4が MC の SMMU 有効ビットを**読んで**記録する（変更はしない）。前提が崩れていたら実機ログで分かる。

## 1. ステージ梯子

| S | 名前 | 内容 | 成功の観測 | 止まりうる所 |
|---|---|---|---|---|
| 0 | MAP | HAL 検証のみ（HW に触れない） | ジャーナルに STAGE_OK(0) | — |
| 1 | POWER | PMC クランプ解除・CAR クロック有効化・リセット解除 | CAR の GPU クロックゲート/リセットのビットが期待値 | **BAR0 に初めて触る直前**（バスハング） |
| 2 | ID | BOOT0 読み出し・照合(0x12b)、PMC_ENABLE、割り込みマスク | `boot0=0x12b000a1` 付近 | 同上 |
| 3 | CLK | GPCPLL（既定は PLL を使わず基準クロック） | PLL ロック or バイパス確認 | PLL 有効化（電圧不足で停止） |
| 4 | PRIV | priv ring・timer・fuse(floorsweep)・FB・L2・top テーブル・SMMU 診断 | GPC/TPC 数 (Tegra X1: 1 GPC・2 TPC 想定) | priv ring 初期化 |
| 5 | MEM | インスタンスメモリ・BAR1・GMMU（VM, 4KiB ページ） | VM に map した DMA 領域を HW 経由で読める | MMU TLB flush 待ち |
| 6 | FIFO | PFIFO/PBDMA/runlist/チャンネル + ホスト専用セマフォ自己テスト | セマフォ書き込みが DRAM に届く | runlist 投入 |
| 7 | ACR | PMU/ACR セキュアブート・FECS/GPCCS 起動 | FECS/GPCCS が応答 | ACR ブート（ファーム必須） |
| 8 | GR | GR 初期化・コンテキスト・compute 自己テスト | compute メソッド完了 | GR コンテキスト |

実行は `cfg.max_stage`（レジストリ `MaxStage`）までで止まる。1 回の検証で 1 段ずつ上げる。

## 2. コアの規約（`core/` と `regs/` に適用。違反は `make check` が落とす）

1. **freestanding C11**。含めてよい標準ヘッダは `<stdint.h> <stddef.h> <stdbool.h> <stdarg.h>` のみ。libc 関数は呼ばない（`gm20b_zero/copy` を使う）。
2. **MMIO は HAL 経由のみ**（`gm20b_rd/gm20b_wr`, `GRD/GWR/GRMW`）。1 回 1 個の 4 バイトアラインド 32bit アクセス。memcpy で MMIO に触らない。
   （アライメント違反は必ず起きる前提。ARM64 Device メモリは非アライン/複数レジスタ転送で例外になる。）
3. **DMA メモリは `gm20b_dma_wr32/rd32/wr64/rd64` 経由のみ**。64bit エントリは「上位 → バリア → 下位」の順（`gm20b_dma_wr64`）。
4. 動的メモリ確保は `gm20b_dma_alloc` のみ。状態は固定長配列。再帰禁止。1 関数のスタック ≤ 256 B 目安（カーネルスタックは小さい）。
5. **全ての待ちは有界**（`gm20b_poll` か、時間と回数の両方で上限のあるループ）。無限ループ禁止。
6. **ジャーナル**: ハングしうる操作（新しく通電/クロック供給したブロックへの最初のアクセス、リセット解除、PLL 有効化、ACR 起動、runlist 投入 …）の
   **直前**に `GM20B_STEP(g, J_Sx_..., a, b)` を書く（記録して即永続化）。ハングしたらジャーナルの最後の行が犯人。
   読み出した診断値は `gm20b_j(g, J_PROBE, tag, value)` で必ず残す（失敗しても情報が最大になるように）。
7. **失敗時は後始末**: `init` が失敗したら実行器が `fini` を呼ぶ。`fini` は部分初期化でも安全であること（状態フラグを持つ）。
   S1 の `fini` は GPU をリセットに戻しクロックを止める（CAR/PMC のみ触る。BAR0 には触らない）。
8. **安全**: 既定で PMIC/電圧に触れない。`GM20B_CF_ALLOW_RAIL` かつ `hal.rail_gpu_set_uv` があり、
   値が `GM20B_RAIL_MIN_UV..MAX_UV` の範囲内のときだけ。SMMU/MC は**読むだけ**。CAR は SET/CLR エイリアスのみ。
9. 戻り値は `GM20B_OK`(0) か負の `GM20B_E_*`。

## 3. 「理論値」の扱い（出典・確度・検証）

全ての**値**（アドレス、ビット、マジック番号、待ち時間、手順の順序）に出典をつける。コード中は
`/* [nvgpu:gk20a/gk20a.c:123] [A] */` のように **出典と確度**を併記し、同じ内容を `docs/provenance/sN_*.md` に表で残す。

| 確度 | 意味 |
|---|---|
| **A** | ベンダー資料（nvgpu の hw ヘッダ/ソース）と nouveau の**両方が一致** |
| **B** | 片方の一次資料のみ、または両者で表現は違うが結論が一致 |
| **C** | 推測・設計判断（例: 待ち時間の上限、安全側の既定値）。**実機で測って確定する**項目 |

provenance の表の列: `項目 | 値 | 出典1(path:line) | 出典2 | 確度 | 実機での確かめ方`。
**一次資料と食い違った場合は黙って選ばず**、両方を記録し、採った方とその理由を書く。

ライセンス: nvgpu(r32.7) と nouveau(nvkm) は MIT。**コードを写す場合は元の著作権表示を保持**し、`NOTICE.md` に追記する。
Linux の `drivers/clk/tegra`・`drivers/soc/tegra` は GPL-2.0: **レジスタ番地・ビット位置という事実の確認にだけ使い、コードは写さない**。
レジスタ定義は `regs/nvgpu/hw_*_gm20b.h`（NVIDIA 無改変）を `#include "nvgpu/hw_xxx_gm20b.h"` で使う（転記しない）。
Tegra の CAR/PMC/MC など nvgpu に無いものは `regs/tegra_*.h` に手書きし、各定義に出典を付ける。

## 4. ディレクトリと所有者

```
core/stageN_*.{c,h}, core/jcodes_sN.h   ← ステージ N の実装者のみが編集
core/gm20b*.{c,h}, core/jcodes 以外の共通  ← 共通基盤（変更したい場合は提案を報告に書く。直接は編集しない）
regs/nvgpu/*                              ← 無改変（編集禁止）
regs/tegra_*.h                            ← 作成したステージの実装者
sim/mock.{c,h}, sim/test*.{c,h}           ← 共通基盤（編集禁止。拡張は新ファイルで: sim/mock_<area>.{c,h}, sim/test_<area>.c）
docs/provenance/sN_*.md                   ← ステージ N の実装者
win/                                       ← Windows シェル担当
```

ジャーナル符号は各ステージの `jcodes_sN.h` に `X(J_SN_NAME, 0xN###)` で足す（範囲 0xN000–0xNfff）。
`gm20b_jcode_name()` が重複値をコンパイルエラーにする。

## 5. テスト

- `make test`: ホストでコアをモック GM20B に対して実行（ASan/UBSan 付き）。`make test T=s1_` で名前絞り込み。
- テストで必ず確かめること: **順序**（例: クランプ解除 → クロック → リセット解除、BAR0 は最後）、
  **ゲート違反ゼロ**（`mock_nviolations()==0`: 通電前の BAR0 アクセスはモックが違反にする）、
  **タイムアウト経路**（ハードが応答しない時に有界で失敗し、後始末される）、
  **意図ログが先**（危険操作の前に STEP が flush 済み: `acc.jlen` と `mock_jfind` で検証）、**DMA リーク無し**。
- モックの機器モデルは**実装とは別の根拠**（nvgpu hw ヘッダのフィールド定義）から書く。実装と同じ表を二度写して「一致した」を見ない。
- `make check-win`: ARM64 Windows 向け freestanding でコンパイルし FP/SIMD 命令ゼロを確認。

## 6. Windows ドライバ（`win/`）

手順書は `docs/WINDOWS.md`。ここは設計の要点だけ。

- レガシー WDM（PnP 無し、デマンド起動のみ、ポーリングのみ、`PASSIVE_LEVEL` のみ、プール確保なし、状態は `.bss` の 1 個）。
  `DriverEntry` が `\Device\Gm20b`（排他）と `\DosDevices\Gm20b` を作る。ブリングアップが失敗しても `STATUS_SUCCESS` を返し、読み込まれたままにする（状態を調べるため）。
- 固定物理アドレス 5 か所（BAR0 / BAR1 / CAR / PMC / MC）を `MmMapIoSpaceEx` で割り当て、HAL（整列 32 ビット MMIO、有界な待ち、非キャッシュの連続 DMA）を実装して
  `gm20b_init()` を呼ぶ。HAL の `rail_gpu_set_uv` は NULL（PMIC に触れない）。`MaxStage` の既定は 0（ハードウェアに触れない）。
- 設定: `HKLM\SYSTEM\CurrentControlSet\Services\gm20b\Parameters` の `MaxStage`・`Flags`・`GpcKhz`・`RailUv`・`PollScalePct`・`AllowRawMmio`（全て REG_DWORD）。
- ジャーナルは二重に永続化する。`\SystemRoot\gm20b-journal.bin`（ライトスルーで追記 + 都度フラッシュ、1 回のロードにつき 2 MiB まで）と、最新の 1 件を
  レジストリ値 `LastEntry` に（`ZwFlushKey`）。シェルの記録（`J_WIN_*`、seq ≥ 0xF0000000）は順序を保つため、コアの古い記録を先にファイルへ流してから積む。
  `STEP` 付きの記録は、ハードウェアに触れる前に永続化される（BSOD / ハング後に SD カードから読める）。
- IOCTL: GET_STATUS / READ_JOURNAL / ADVANCE / FINI / RAW_READ / RAW_WRITE。直列化（同時なら `STATUS_DEVICE_BUSY`）。生レジスタは `AllowRawMmio=1` のときだけ。
  MC は書けず、CAR は SET/CLR エイリアスだけ書け、PMC は GPU_RG_CNTRL だけ書ける。BAR は通電確認つき（確認が CAR/PMC を読む前にも `STEP` を残す）。
  ユーザーモードのツール `gm20bctl.exe` がこれらを操作する。
- 検査（`make -C win check`）: PE 構造、FP/SIMD 命令なし、構造体レイアウト 83 項目を mingw-w64 の DDK ヘッダと照合、ジャーナルコードの一意性、
  偽の ntoskrnl 上でのシェル全体テスト（レジスタ窓は mmap でトラップし、各ページの最初のアクセスの時点でファイルとレジストリのどこまでが永続化済みかを記録）、
  シェルが作ったジャーナルを PC 側デコーダで読む検査（擬似ハングから原因行が出る）。
- 変異テスト（`make -C win mutate`、`win/tests/mutate.py`）: ガードを 1 つずつ壊した複製でテストが落ちることを確かめる。ガードを足したら変異も足す。
- Windows 上でのロードは**未検証**。
