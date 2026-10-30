# 統合ブランチ `unified` (Under-development 5a32737 から)

## 変えたこと
| 内容 | 場所 |
|---|---|
| SDM1 を nvtsdhc 用に記述するスイッチ `SWITCH_ACPI_SD_NVTSDHC` (Windows 11) | `AcpiTables/Dsdt/Dsdt.asl`, `NintendoSwitch.dsc` |
| `UsbPowerAuditDxe` を hayato41200 の R379 に更新 (I2C2 / SPI4 のクロックとセンサー ID 読み出し) | `Drivers/UsbPowerAuditDxe/UsbPowerAudit.c` (出典コメントのみ winx 側) |
| DSDT に `I2C2` と `SENS`(SWOA0901) を追加 (`SWITCH_ACPI_SWOA` のときだけ) | `AcpiTables/Dsdt/Dsdt.asl` |
| `WindowsDrivers/` を Windows-for-Switch-Drivers main に更新 (Tegra210Sdhc, SwitchSensor, SwitchPatcher, Switch-Sd, Switch-Sensor) | `WindowsDrivers/` |
| ビルドの入口を `Tools/build.sh` に一本化 (既定 `woa`、従来の構成は `ehci`) | `Tools/build.sh` |
| 番号付きパッチ群 `patches/1..6` を移動 | `archive/patches/` |

## ビルド
```
Tools/build.sh                 # woa (hekate + Windows-for-Switch スタック)
SD=nvtsdhc Tools/build.sh      # + Windows 11 のSD用 (nvtsdhc)。Windows 10 は付けない
Tools/build.sh ehci            # 従来の EHCI 構成
```
- `Firmware/tegra21x_xusb_firmware` は各自で用意する (`Firmware/README.md`)。
- `SD=nvtsdhc` のときは `SDLOADAA64*.EFI` を ESP に置かない (SD ホストが二重定義になる)。

## 注意 (未検証)
- ビルドも実機での起動も、まだ確かめていない。DSDT は iasl でのコンパイル未確認。
- SENS を足したので、UEFI は R379 以降のこと。R378 の UEFI に戻すときは `SWITCH_ACPI_SWOA` の `I2C2`/`SENS` を外す
  (SPI4 は、クロックが止まっていると読み取りで固まる)。
- hekate 経由で `UsbPowerAuditDxe` が `STOP:` する可能性は、従来どおり未検証。
- `SWITCH_WOA` と `SWITCH_USB`(EHCI) は、まだ併用できない。同居させるかどうかは、実機の結果を見てから決める。
- DSC/FDF の条件の食い違い (PciEmulation を DSC では常にビルド、FDF では `SWITCH_USB` のときだけ入れる、など) は、無害なので変えていない。
