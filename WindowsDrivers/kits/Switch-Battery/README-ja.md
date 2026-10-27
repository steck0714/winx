# バッテリー表示：Nintendo Switch のバッテリーのドライバー（battc）

## v4（1.0.0.4、2026-10-26）: USB-C の抜き差しへの対応

- **前の問題:** UEFI が USB-C の役割を決めるのは、起動したときの1回だけだった。そのため、何も挿さずに起動すると、Windows で USB 機器を挿しても反応しなかった。
- **UEFI R378 の変更:**
  - 何もつながっていなくても、XUSB を起動したまま Windows に渡す。
  - DSDT の BAT0 に、3つ目の接続として BM92T（I2C1 の 0x18）を足した。
- **v4 の変更:** 0.5 秒ごとに BM92T を見張り、UEFI と同じ判断（3回続けて同じ状態）で切り替える。
  - **USB 機器（OTG アダプター、電源の無いハブ）を挿したとき:** BQ24193 を OTG モードにして、本体から 5V を出す。抜くと（2回続けて見えなくなると）、5V を止める。
  - **充電器付きのハブを挿したとき:** UEFI の R363 と同じ手順で、ホストとして使えるようにする。
    1. 充電モードにする。
    2. PD の契約から、入力電流の上限を決める。
    3. 相手が DRD なら、DR_SWAP でホストになる。
  - **安全策:** 相手から電気が来ている（VSAFE、または BQ の VBUS 状態が「有り」）ときは、5V を出さない。
- **記録:** `UsbcState`（0 無し、1 本体から 5V、2 充電）、`UsbcChanges`、`UsbcContractMv`、`UsbcInputLimitMa`、`UsbcDataRole` など。
- **組み合わせ:** UEFI R377 以前で使うと、BM92T の接続が無いので、今までと同じ動き（バッテリー表示だけ）になる。

## 中身

| ファイル | 内容 |
|---|---|
| `switchbattery.sys` | バッテリーのミニクラスドライバー（KMDF 1.15、battc、ARM64、テスト署名）。ソースは `..\SwitchBattery\src\switchbattery.c`（Microsoft の SimBatt サンプルの構成） |
| `switchbattery.inf` | `ACPI\SWOA0301`（Class=Battery、`battery.inf` の Battery_Inst を使う） |
| `Battery.ps1` | Install、Status、Remove |

## ドライバーの作り（v1）

- **I2C の接続:** DSDT の `BAT0` に、I2cSerialBus が2つある。0 が残量計の MAX17050（0x36）、1 が充電 IC の BQ24193（0x6B）。どちらも I2C1 上。段階3の I2C ドライバーを経由して読む（`IOCTL_SPB_EXECUTE_SEQUENCE` で、レジスター番号を書いてから続けて読む）。
- **残量（相対値で報告）:** MAX17050 の RepSOC（0x06 の上位バイト、%）。`BATTERY_CAPACITY_RELATIVE` で、満充電を 100 とする。
- **電圧:** VCELL（0x09、(raw>>3) × 0.625 mV）。
- **温度:** TEMP（0x08、1/256 ℃）。
- **AC の接続と充電中かどうか:** BQ24193 の SYS_STAT（0x08）から判断する。VBUS_STAT が 1（USB host）か 2（adapter）で、かつ PG なら AC あり。CHRG_STAT が 1 か 2 なら充電中。**OTG（3）は、Switch が VBUS を出している状態なので、AC なしとして扱う。**
- **放電率:** UNKNOWN にする（残り時間の推定は、まだ出さない）。
- **状態の更新:** 10 秒ごとに読み、AC の状態か残量が変わったら `BatteryClassStatusNotify` で Windows に知らせる。
- **CRITICAL フラグは立てない。** 残量が少なくなったときの動作（休止状態やシャットダウン）は、Windows の電源プランの設定に任せる。
- **記録:** `Services\switchbattery\Parameters` に、接続の数、開いたときの結果、最初に読んだ値、最後の値を書く。

## 手順

1. **Switch を UEFI R371 で起動する。** ペイロードは `D:\Codex\coreboot-R371.rom`。GPIO、ボタン、I2C、BAT0 が入っている。段階3の I2C ドライバーが入っている必要がある。
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\Battery`。
3. **インストールする。**
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Battery\Battery.ps1 -Mode Install
   ```
4. **確かめる。**
   - タスクバーにバッテリーのアイコンが出て、残量（%）と「電源に接続」「充電中」が表示されるか。
   - スクリプトの出力に `device: ACPI\SWOA0301\0 ... status=OK` と `Win32_Battery: charge=..%` が出るか。
5. **結果を共有する。** `C:\SwitchWOA-Battery.txt` を見せてほしい。`C:\SwitchWOA-battery-report.html`（powercfg のバッテリーレポート）もあればなおよい。

## 結果（2026-10-25）: バッテリー表示は完了

- **switchbattery v2 と tegra210i2c v3:** `ACPI\SWOA0301\0` が **status=OK**。タスクバーにバッテリーのアイコン（電源接続）が出た。
- **Win32_Battery:** charge=100%、status=2（AC）、voltage=4206 mV。`powercfg /batteryreport` も作成できた。
- **driver trace（最初に読んだ値）:** `FirstCapacity=0x51`（81%）、`FirstVoltage=0x1070`（4208 mV）、`FirstPowerState=5`（AC オン + 充電中）、`ClassInitStatus=0`。

### v3（1.0.0.3）: AC の抜き差しが反映されるようになった

- **v2 の問題:** 充電器（ハブ）を抜いても「電源接続」の表示のままだった。
- **原因:** 10 秒ごとに読み直すタイマーを、PASSIVE レベルの周期タイマーとして作っていた。KMDF ではこれを作れないため、タイマーが一度も動いていなかった（`LastPowerState` などの記録が無かった）。
- **修正:** 単発のタイマーにして、処理のたびにかけ直す形にした。
- **結果:** ユーザーの確認で、**抜き差しが表示に反映されるようになった**。
- **残り:** I2C と GPIO の定期記録のタイマーも同じ作りなので、動いていない（記録用なので、動作には影響しない）。

### 途中でわかったこと（I2C ドライバーの v1〜v2 で接続が失敗した原因）

この Windows（19041/19044）で SpbCx が渡す接続パラメーターは、**先頭に ULONG が1つ多い**。
- `ConnectRaw0=0x1`: 先頭の ULONG。たぶん版番号。
- `ConnectRaw1=0x1C`: 本当の PropertiesLength（28）。
- **8 バイト目から** ACPI の GenericSerialBus の記述子が始まる（`8E 19 00 02 ...`、I2C、アドレス 0x6B、100000 Hz）。

Microsoft のサンプルの `RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER`（長さ + 記述子）として読むと、長さが 1 になって弾いてしまう。v3 は、タグ 0x8E を探して読むので、この並びでも動く。

## もし起動しなくなったら

R370 の ROM で起動すれば、BAT0 が無い状態に戻る。その状態で `-Mode Remove` を実行する。
