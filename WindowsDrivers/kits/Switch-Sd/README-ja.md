# Switch-Sd: SD から Windows を起動するためのファイル

SwitchPatcher が SD に入れるファイルです。手で入れる必要はありません。

| ファイル | 使う SD | 内容 |
|---|---|---|
| `SDLOADAA64.EFI` | Windows 10 | ESP の `EFI\Boot\bootaa64.efi` に置くローダー。SD ホスト（`\_SB.SDH0`、`ACPI\NVDA0101`、互換 ID `PNP0D40`）の SSDT を足してから `bootmgfw.efi` を起動します。Windows 標準の sdbus が SD を読みます。 |
| `SDLOADAA64-NOCID.EFI` | Windows 11 | 上と同じで、互換 ID だけ `NVD0D40` に変えたもの。標準の sdbus は結び付かず、nvtsdhc だけが SD を読みます。 |
| `nvtsdhc.sys` / `.inf` / `.cat` | Windows 11 | Tegra210 の SD ホストの sdport ミニポート（ADMA2、32 ビットアドレス）。起動時に読み込みます（Start = 0）。テスト署名なので、テストモードが必要です。 |

- ローダーのソースは `drivers/Tegra210Sdhc/loader/sd-acpi-chainloader.zig` です。NOCID 版は、SSDT の互換 ID の 4 バイト（`PNP0` → `NVD0`）と、表のチェックサムを書き換えたものです。
- Windows 11 22621 では、標準の sdbus は SD から起動できません（DMA の扱いが合わず、起動ドライブを読めません）。Windows 10 は標準の sdbus で起動します。
- UEFI の DSDT に SD ホストを足すときは、ローダーを外してください（`\_SB.SDH0` が二重になります）。
