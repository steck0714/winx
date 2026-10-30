# 明るさセンサーと傾きセンサー：Nintendo Switch のセンサードライバー

コントロールセンター 0.6.0 以降の「明るさの自動調整」と「画面の自動回転」は、このドライバーの値を使います。

## 中身

| ファイル | 内容 |
|---|---|
| `switchsensor.sys` | KMDF ドライバー（ARM64、テスト署名）。ソースは `drivers\SwitchSensor\src\switchsensor.c` |
| `switchsensor.inf` | `ACPI\SWOA0901` |
| `Sensor.ps1` | Install、Status、Watch、Remove |

## ドライバーの作り

- **使うリソース（DSDT の `SENS`、UEFI R379 以降）:**
  - SPI4 コントローラー（0x7000DA00）。CS0 に IMU（LSM6DS3H）がつながっている。SPI モード 3。
  - I2C2 上の明るさセンサー BH1730FVC（0x29）。
- **前提:** UEFI R379 が、I2C2 と SPI4 のクロックとピンを設定する。明るさセンサーの電源（LDO6）は、タッチパネル用に R373 から入っている。
- **明るさ:**
  - 積分時間は 38 サイクル（102.6 ms）に固定する。
  - ゲイン（x1、x2、x64、x128）は、値に合わせて自動で切り替える。
  - lux は、データシートの式と、L4T の DT にある Switch の窓の係数で計算する。
- **傾き:** 加速度を 104 Hz、±2 g で測る。値は mg（チップの軸のまま）。
- **読み方:** 200 ms ごとに両方を読み、最後の値を `\\.\SwitchSensor` の IOCTL（0x00226080）で返す。ログイン中のユーザーも読める。
- **片方が動かなくても、もう片方は使える。** 明るさセンサーが10回続けて答えないときは、読むのをやめる。
- **記録:** `Services\switchsensor\Parameters` に次を書く。
  - 起動したときの結果: AlsPartId、ImuWhoAmI、AlsOk、ImuOk など。
  - 10秒ごとの値: LuxMilli、AlsGain、AccelX/Y/Z、Samples。

## 手順

1. **Switch を UEFI R379 で起動する。** ペイロードは `D:\Codex\coreboot-R379.rom`。
   - UEFI の画面に、次の2行が出るか見る。
     - `R379 light sensor: ... BH1730 PART_ID read 7x`
     - `R379 IMU: ... WHO_AM_I read 69`
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\Sensor`。
3. **インストールする。** 管理者の PowerShell で実行する。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Sensor\Sensor.ps1 -Mode Install
   ```
   `AlsOk=0x1` と `ImuOk=0x1` が出れば成功。
4. **コントロールセンターの「画面」タブで試す。**
   - 明るさセンサーと傾きセンサーの値が、0.5秒ごとに出る。
   - 「明るさを自動で調整する」と「画面を自動で回転する」を入れる。

## 画面の自動回転

- 自動回転を入れたときの画面の向きが横向きなら、それを「ふつうに持ったときの向き」として覚える。
- 回る向きが違うときは、次のどちらかで直す。
  - **向きがずれている:** 正しい向きに画面を手で直してから、「今の画面の向きを、今の持ち方の正しい向きとして覚える」を押す。
  - **逆に回る:** 「回転の向きを逆にする」を入れる。
- 机に平らに置いたときは、回さない。向きは、1秒続いてから変える。

## 明るさの自動調整

- 暗い所では 12%、明るい所（約 1000 lux 以上）では 100% にする。その間は、lux の対数に合わせて変える。
- 少しの変化では変えない。変えるときは、少しずつ変える。
- 明るさを手で変えると、自動調整は切れる。
