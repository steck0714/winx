# S2(ID)— 触るレジスタと根拠

ステージ 2 は **最初の BAR0 アクセス**です。電源の理論が間違っていれば、ここでバスハングとして現れます。実装は `core/stage2_id.c`。
レジスタの事実は同梱している NVIDIA nvgpu のヘッダ(`regs/nvgpu/hw_mc_gm20b.h`、MIT)から取っています。
確度: **A** = 一次資料(MIT)に直接ある / **B** = 一次資料 1 件以上で確認 / **C** = 推定 / 設計判断。

## 流れ

1. **ガード**: CAR / PMC を読み直し、「通電していない」と出たら BAR0 には触れずに `GM20B_E_STATE`(`J_S2_GATED`)。
   `GM20B_CF_FORCE_BAR0`(`Flags` の `0x10000`)でこのガードだけを飛ばせる(CAR/PMC の読み方の方を疑うとき。飛ばしたことは `J_S2_FORCED` に残る)。
2. **BOOT0**: `J_S2_BOOT0_INTENT`(STEP)を永続化してから BOOT0 を読み、直後に結果 `J_S2_BOOT0` も永続化する。
3. **不正値の判定**: `0xffffffff`・`0xbadfXXXX`(バスエラーのパターン)は `GM20B_E_IO`、`0` は `GM20B_E_NODEV`(`J_S2_BAD_BOOT0`)。
4. **デコード**: チップセット = (architecture << 4) \| implementation。nouveau の式 `(boot0 & 0x1ff00000) >> 20` と一致しなければ失敗。
   GM20B(0x12b)でなければ `J_S2_WRONG_CHIP`。
5. **PMC_ENABLE**: 読んで記録するだけ(何も有効にしない)。
6. **割り込み**: MC の割り込みレジスタ 4 本を記録(`J_S2_INTR_PRE`)。DIAG_ONLY ならここで終了。そうでなければ全て「無効」にして書き込み、
   読み戻して記録(`J_S2_INTR_POST`)。まだ立っているものがあれば `J_S2_INTR_NOT_MASKED`(致命的ではない。誰も処理しないので)。

fini は何もしません(マスクした割り込みが安全側の状態だから)。

## レジスタ表(BAR0 = 0x57000000 からのオフセット)

| レジスタ | オフセット | 用途 | S2 の操作 | 出典 | 確度 | 実機での確かめ方 |
|---|---|---|---|---|---|---|
| `mc_boot_0` | 0x0 | チップ ID。期待値 **0x12b000a1** | 読み(最初の BAR0 アクセス) | nvgpu `hw_mc_gm20b.h` | A | `rawread bar0 0`(`AllowRawMmio=1` が要る) |
| BOOT0 の architecture | [28:24] = 0x12 | | デコード | nvgpu `mc_boot_0_architecture_v` | A | |
| BOOT0 の implementation | [23:20] = 0xb | | デコード | nvgpu `mc_boot_0_implementation_v` | A | |
| BOOT0 の major / minor revision | 0xa / 0x1(期待値の `a1`) | 参考(記録のみ) | デコード | nvgpu | A(位置)/ B(期待値) | |
| チップセット照合 | 0x12b | `(arch << 4) \| impl` と nouveau の `(boot0 & 0x1ff00000) >> 20` が一致 | 照合 | nvgpu + nouveau `nvkm_device_ctor` | A | `J_S2_DECODE a=12b b=12b` |
| 不正パターン | 0xffffffff / 0xbadfXXXX | バスエラー応答の目印 | 判定 | 設計(nvgpu の bad-bus 値の知識) | C | |
| `mc_enable` | 0x200 | エンジンの有効ビット | 読みのみ | nvgpu `mc_enable_r` | A | |
| `mc_intr_en_0` / `mc_intr_en_1` | 0x140 / 0x144 | 割り込み許可 | 読み、**書き**(`inta_disabled`) | nvgpu | 番地 A / 「全部無効にする」方針 C | |
| `mc_intr_mask_0` / `mc_intr_mask_1` | 0x640 / 0x644 | 割り込みマスク | 読み、**書き**(0) | nvgpu | 番地 A / 方針 C | |
| ガードが読む CAR / PMC | CAR+0x280 / +0x28c、PMC+0x2d4 | 通電判定(S1 の検証と同じ 3 条件) | 読み | `s1_power.md` | B | |

書き込みは割り込みレジスタ 4 本だけで、BOOT0 が GM20B と確認できた後でしか行いません(`DIAG_ONLY` では書かない)。

## ジャーナルコード(主なもの)

| コード | 内容 |
|---|---|
| `J_S2_GUARD_CLK` / `_RST` / `_CLAMP` | ガードが読んだ 3 つの値 |
| `J_S2_GATED`(a = ビット: 1 クロック / 2 リセット / 4 クランプ) | 通電していないと判断して BAR0 に触れず停止 |
| `J_S2_FORCED` | ガードを飛ばして続行 |
| `J_S2_BOOT0_INTENT`(STEP) | **最初の BAR0 アクセスの直前**。ハング時の最終行がこれなら BOOT0 の読み出しで止まった |
| `J_S2_BOOT0`(STEP) | 読み出し結果 |
| `J_S2_BAD_BOOT0` | 不正値(a = BOOT0、b = エラー) |
| `J_S2_DECODE` | a = チップセット、b = nouveau 式の値 |
| `J_S2_WRONG_CHIP` | GM20B ではなかった |
| `J_S2_PMC_ENABLE` | PMC_ENABLE の値 |
| `J_S2_INTR_PRE` / `_MASK` / `_POST` / `_NOT_MASKED` | 割り込みレジスタの事前値 / 書き込み / 事後値 / 残り |
| `J_S2_DIAG_ONLY` | 読むだけで終了 |

## 実機で最初に確かめること(土曜)

1. BOOT0 が 0x12b000a1 か(違うなら `J_S2_WRONG_CHIP` で窓の物理アドレスを疑う)。
2. 0xffffffff / 0xbadfXXXX / ハングなら電源レール(`WINDOWS.md` §8 の 1)。S1 の `POST_*` と `PRE_*` を併せて見る。
3. `mc_enable` と割り込みレジスタの事前値(何が既に動いているかの手がかり)。
