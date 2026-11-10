# regs/nvgpu — NVIDIA の GM20B レジスタ定義（無改変）

出典: github.com/OE4T/linux-nvgpu  branch `l4t/l4t-r32.7.6`
      `drivers/gpu/nvgpu/include/nvgpu/hw/gm20b/hw_*_gm20b.h`
コミット: 7fbefcbf4b39643ab414454e6e4154865b333757
ライセンス: MIT（各ファイル先頭の NVIDIA の許諾文をそのまま保持。改変していない）

使い方: `#include "nvgpu/hw_mc_gm20b.h"` のように必要なものだけ取り込む（`-Iregs`）。
命名規則（各ファイル冒頭のコメントより）:
  <x>_r()  レジスタのオフセット      <x>_o()  要素のオフセット    <x>_w()  ワードオフセット
  <x>_v(r) レジスタ値から値を抽出     <x>_m()  マスク              <x>_s()  ビット幅
  <x>_f(v) 値からフィールドを作る     <x>_<name>_v() / _f()        列挙値
全て `static inline` なので、未使用でも警告は出ず、タイプミスはコンパイルエラーになる。
これが「理論値」の最も信頼できる一次資料（NVIDIA 自身のドライバが使っている定義）。
