# NOTICE

このリポジトリ自身のコードは、各ファイル先頭の `SPDX-License-Identifier`(現状 MIT)に従います。
以下は、取り込んだ・参照した第三者の資料と、その扱いです。出典のコミットは `docs/provenance/SOURCES.md`。

## 1. NVIDIA nvgpu(L4T r32.7.6)— MIT — 同梱

`regs/nvgpu/hw_*.h`(26 ファイル)は NVIDIA の Tegra GPU ドライバ(nvgpu)のレジスタ定義ヘッダです。
各ファイルの先頭に著作権表示と許諾文を保持しています(年は 2014〜2023 の範囲でファイルごとに異なります)。許諾文は次のとおりです:

> Copyright (c) 2014-2017, NVIDIA CORPORATION.  All rights reserved.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files
> (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge,
> publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
> subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
> MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
> FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
> WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

(冒頭の年は 2014-2017 の例。ファイルごとの表示が正です。)
nvgpu の GPL のファイル(`common/io_common.c`、`common/nvlink.c`)は**使っていません**。

## 2. nouveau(Linux の nvkm)— MIT — 参照のみ

nouveau の `nvkm/engine/device/tegra.c`(電源投入の**順序**)と `nvkm_device_ctor`(BOOT0 の**デコード式**)を参照しました。
コードは写していません。

## 3. Linux の Tegra クロック / PMC ドライバ — GPL-2.0 — 事実の確認のみ

`drivers/clk/tegra/*`、`drivers/soc/tegra/pmc.c`、`include/dt-bindings/*` は GPL-2.0 です。
**レジスタ番地とビット位置という事実の確認にのみ**使い、コードは写していません(該当箇所は `regs/tegra_car_pmc.h` と `docs/provenance/s1_power.md` に
ファイル名と行番号で記録)。

## 4. 同梱していないもの

- NVIDIA のファームウェア(ACR / FECS / GPCCS / PMU の blob): 同梱しません。S7 以降で必要になるので、利用者が自分で用意します。
- `bl33.bin`(利用者の UEFI): 読み取り専用で解析しただけで、同梱も改変もしていません(`DESIGN.md` §0)。
- 署名用の鍵・証明書: 生成も同梱もしません。署名は利用者自身のテスト証明書で行います(`docs/WINDOWS.md` §2)。
- Windows SDK / WDK のヘッダやライブラリ: 使っていません。`win/include/ntk_min.h` は必要な型・定数・21 個の関数宣言を自前で書いたもので、
  構造体の配置は mingw-w64 の DDK ヘッダ(x86_64)と突き合わせて検査しています(`make -C win check-layout`)。
