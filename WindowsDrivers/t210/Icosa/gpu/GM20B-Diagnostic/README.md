# winx-gm20b

Nintendo Switch(Tegra X1)の GPU **GM20B** を、Windows 11 ARM64 から段階的に立ち上げるための実験用ドライバ。
**理論値**(NVIDIA nvgpu r32.7.6 と nouveau の一次資料)で作ってあり、**実機では未検証**、**Windows 上で一度もロードされていません**。

| ステージ | 内容 | 状態 |
|---|---|---|
| S0 MAP | レジスタ窓を張る | 実装 |
| S1 POWER | CAR / PMC でクロック・リセット・クランプ | 実装 |
| S2 ID | 通電確認 → BOOT0 → 割り込みをマスク | 実装 |
| S3〜S8 | クロック(GPCPLL)/ priv ring・FB / GMMU・BAR1 / PFIFO / ACR・FECS・GPCCS / GR | **未実装**(スタブが `NOTIMPL`) |

## 読む順番

1. `docs/WINDOWS.md` — 手順書(土曜の手順、ジャーナルの読み方、署名、未検証リスク)
2. `docs/STATUS.md` — 現状と次にやること(再開用)
3. `docs/DESIGN.md` — 設計(ステージ梯子、コアの規約、理論値の扱い)
4. `docs/provenance/` — レジスタごとの出典・確度・実機での確かめ方

## 作る・検査する

```
make check              # 全検査(コアのテスト、Windows ARM64 のコンパイル、PE 構造、レイアウト照合、偽 ntoskrnl 上のシェルテスト)
make -C win dist        # build/dist/gm20b-win/ に配布一式(gm20b.sys は未署名)
make -C win mutate      # 変異テスト(わざとバグを入れてテストが落ちるか。約 1〜2 分)
```

必要な物: clang 18 以上、lld-link、llvm-dlltool、python3。WDK も Windows SDK も不要。

## 構成

| ディレクトリ | 内容 |
|---|---|
| `core/` | フリースタンディング C11 のコア(OS 非依存。MMIO は HAL 経由のみ) |
| `regs/` | レジスタ定義(`nvgpu/` は NVIDIA の MIT ヘッダ) |
| `sim/` | 模擬 GM20B チップとコアのテスト |
| `win/` | Windows シェル(`gm20b.sys`、`gm20bctl.exe`、スクリプト、テスト、デコーダ) |
| `docs/` | 設計・手順書・出典 |

ライセンスと第三者の資料の扱いは `NOTICE.md`。
