# S1(POWER)— 触るレジスタと根拠

ステージ 1 は **CAR と PMC だけ**を触り、BAR0 には触れません。実装は `core/stage1_power.c`、レジスタ定義は `regs/tegra_car_pmc.h`。

出典の扱い:

- **順序**は nouveau の `nvkm_device_tegra_power_up()`(`nvkm/engine/device/tegra.c`、MIT)。
- **レジスタ番地とビット位置**は Linux の `drivers/clk/tegra` と `drivers/soc/tegra`(GPL-2.0)で確認した。これらは**事実の確認にのみ**使い、コードは写していない。
- 確度: **A** = 一次資料(MIT)に直接ある / **B** = 一次資料 1 件以上で確認 / **C** = 推定。詳細は `DESIGN.md` §3。

## 順序(nouveau、確度 B)

regulator → クロック "gpu" + "ref" + "pwr"(204 MHz) → 10 µs → リセット保持 → 10 µs → クランプ解除 → 10 µs → リセット解除 → 10 µs

Linux との意図的な違い:

1. clk / reset / regulator の枠組みが無いので、CAR は**書き込み専用の SET/CLR エイリアスだけ**で書く(CAR は ACPI の `SWOA0701`「CPUC」と共有。共有レジスタの読み-修正-書きはしない)。
2. `pwr` クロック(PLLP_OUT5、これも CPUC と共有)は**読んで記録するだけ**。S1〜S6 は PMU を使わないので要らない。S7 は必要とする。
3. 電源レール(MAX77621)は、プラットフォームが明示的に許したときだけ(`GM20B_CF_ALLOW_RAIL` + `hal.rail_gpu_set_uv` が非 NULL + 800000〜1050000 µV)。Windows 版はフックを持たないので**触らない**。
   電圧窓は設計上の安全側の値(確度 C)。

## レジスタ表

CAR のベースは 0x60006000、PMC のベースは 0x7000e400(窓の先頭。表のオフセットはその窓内)。

| レジスタ | オフセット | 用途 | S1 の操作 | 出典 | 確度 | 実機での確かめ方 |
|---|---|---|---|---|---|---|
| CAR `CLK_OUT_ENB_X` | CAR+0x280 | クロックゲート(bit24 = GPU、bit29 = PLLG_REF)。1 = 有効 | 読み | linux `clk.h:18` | B | `gm20bctl rawread car 0x280` |
| CAR `CLK_OUT_ENB_SET_X` | CAR+0x284 | 1 を書いたビットのゲートを開く(書き込み専用) | **書き**(GPU \| PLLG_REF) | `clk.h:31` | B | 書いた後に 0x280 の bit24 / bit29 が 1 |
| CAR `CLK_OUT_ENB_CLR_X` | CAR+0x288 | 1 を書いたビットのゲートを閉じる(書き込み専用) | **書き**(fini で、**自分が開けたビットだけ**) | `clk.h:32` | B | fini 後に 0x280 の対応ビットが 0 |
| CAR `RST_DEVICES_X` | CAR+0x28c | 1 = モジュールをリセット保持(bit24 = GPU) | 読み | `clk.h:41` | B | `rawread car 0x28c` |
| CAR `RST_DEVICES_SET_X` | CAR+0x290 | リセットをかける(書き込み専用) | **書き**(GPU) | `clk.h:53` | B | 0x28c の bit24 が 1 |
| CAR `RST_DEVICES_CLR_X` | CAR+0x294 | リセットを解除する(書き込み専用) | **書き**(GPU) | `clk.h:54` | B | 0x28c の bit24 が 0 |
| bit 位置 24 / 29 | — | GPU = id 184 − 160、pllg_ref = id 189 − 160 | — | `clk-tegra-periph.c:806-807`、dt-bindings `TEGRA210_CLK_GPU=184`、`tegra210.dtsi` の gpu ノード(clocks / resets) | B | 上の 3 行 |
| CAR `PLLP_BASE` | CAR+0x0a0 | PLLP の状態(参考) | 読み | `clk-tegra210.c:64` | 番地 B / デコード C | `rawread car 0xa0`(生の値を共有) |
| CAR `PLLP_OUTC` | CAR+0x67c | PLLP_OUT5 = `pwr` クロック | 読み | `clk-tegra-periph.c:257, 857` | 番地 B / 有効ビット(rst_shift + 1)と除数式 C | `rawread car 0x67c` |
| PMC `GPU_RG_CNTRL` | PMC+0x2d4 | bit0 = 1: クランプ / ゲート中。0 を書くとクランプ解除 | 読み、**書き**(0) | `pmc.c:176`(オフセット)、`:594`(通電 ⇔ bit0 = 0)、`:726-727`(0 を書く)、`:3891`(Tegra210 は `has_gpu_clamps`) | B | `rawread pmc 0x2d4`、書いた後 bit0 = 0 |

書き込みは全部で 4 回: `CLK_OUT_ENB_SET_X` → `RST_DEVICES_SET_X` → `GPU_RG_CNTRL`(0)→ `RST_DEVICES_CLR_X`。各書き込みの前に `STEP`
(フラッシュ済みの意図記録)を残し、4 つの手順の間に 10 µs を置く(nouveau に合わせた値。10 µs が十分かは確度 C)。

## 検証(`verify_powered`)

書き込みの後に 3 つを読み戻す。全て満たさなければ `J_S1_VERIFY_FAIL`(a のビット: 1 = クロックゲートが閉じたまま、2 = リセットのまま、4 = クランプのまま)で
`GM20B_E_HW`、続いて fini で巻き戻す。

| 条件 | 値 |
|---|---|
| `CLK_OUT_ENB_X` の bit24 | 1 |
| `RST_DEVICES_X` の bit24 | 0 |
| `GPU_RG_CNTRL` の bit0 | 0 |

**注意: この検証は SoC 内のビットしか見ません。GPU の電源レールが入っているかは分かりません**(`WINDOWS.md` §8 の 1)。

## fini(巻き戻し)

1. `RST_DEVICES_SET_X` に GPU を書く(GPU をリセット保持に戻す)。
2. `CLK_OUT_ENB_CLR_X` に、**S1 が自分で有効にしたビットだけ**を書く(最初から有効だったクロックは閉じない)。
3. クランプは戻さない(GPU を既に使っていた他の主体を壊さないため。リセット保持で安全側)。

S1 が何も書いていなければ fini は何もしない。

## ジャーナルコード(主なもの。全体は `jcodes.tsv`)

| コード | 内容 |
|---|---|
| `J_S1_PROBE_BEGIN`(STEP) | 最初の CAR/PMC 読み出しの前 |
| `J_S1_PROBE_RD`(STEP) | 5 本の事前読み出しの**それぞれの前**。a = (空間 << 28 \| オフセット)。ハング時の最終行がこれなら、そのレジスタが原因 |
| `J_S1_PRE_CLK` / `_RST` / `_CLAMP` / `_PLLP_BASE` / `_PLLP_OUTC` | 事前状態の値 |
| `J_S1_PWR_CLK` | PLLP_OUT5 が 204 MHz 付近で動いて見えるか(参考、確度 C) |
| `J_S1_DIAG_ONLY` | DIAG_ONLY で、読むだけで終了 |
| `J_S1_CLK_ENABLE` / `J_S1_RST_ASSERT` / `J_S1_CLAMP_REMOVE` / `J_S1_RST_DEASSERT`(STEP) | 4 回の書き込みの前 |
| `J_S1_POST_CLK` / `_RST` / `_CLAMP` / `J_S1_PLLG_REF` | 書き込み後の読み戻し |
| `J_S1_VERIFY_FAIL`(STEP) | 検証失敗(a = 失敗マスク) |
| `J_S1_FINI_RST` / `J_S1_FINI_CLK`(STEP)/ `J_S1_FINI_POST` | 巻き戻し |

## 実機で最初に確かめること(土曜)

1. `set-stage.cmd 1 10`(読むだけ): 5 本が読めるか。事前状態(特に `PRE_CLAMP` の bit0)。
2. `set-stage.cmd 1 0`: `POST_*` が §検証の 3 条件を満たすか。満たさなければ `J_S1_VERIFY_FAIL` の a。
3. 特に PMC の書き込みが効くか(a = 4 なら効いていない)。CAR が CPUC の使う領域と同じ窓から問題なく読み書きできるか。
