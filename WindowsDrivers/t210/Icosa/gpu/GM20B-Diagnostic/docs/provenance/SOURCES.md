# 参照した一次資料（取得時点のコミット固定）

| 略称 | 内容 | 場所 | コミット | ライセンス |
|---|---|---|---|---|
| nvgpu | NVIDIA純正Tegra GPUドライバ(L4T r32.7.6) のGM20B/GK20A共通部分 | github.com/OE4T/linux-nvgpu  branch l4t/l4t-r32.7.6 | 7fbefcbf4b39643ab414454e6e4154865b333757 | MIT（hw_*.h と gm20b/gk20a/common の .c は MIT 許諾文を確認。例外: common/io_common.c, common/nvlink.c は GPL → 不使用） |
| nouveau | Linux nouveau (nvkm) のGM20B経路 | github.com/torvalds/linux | 05441d697f7f9a927264f4058cbbc6a03dfcac9d | MIT（nvkm 配下）。tegra クロック等 drivers/clk/tegra, drivers/soc/tegra は GPL-2.0: 「レジスタ番地・ビット位置という事実」の確認にのみ使用し、コードは写していない |
| bl33 | ユーザー提供の現行UEFI(bl33.bin)。読み取り専用で解析 | (添付) | sha256 b28d8c3337e69121e1965372cebabfe015f7f25695b986187806c428721494de | — |
