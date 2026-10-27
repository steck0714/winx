# 電源ボタン：Nintendo Switch の電源ボタンドライバー

## 中身

| ファイル | 内容 |
|---|---|
| `switchpwrbtn.sys` | KMDF ドライバー（ARM64、テスト署名）。ソースは `..\SwitchPowerButton\src\switchpwrbtn.c` |
| `switchpwrbtn.inf` | `ACPI\SWOA0601`。下に Windows 標準の `vhf.sys` を付ける（タッチと同じ） |
| `PowerButton.ps1` | Install、Status、Watch、SetAction、Remove |

## ドライバーの作り

- **ボタンのつながり:** 電源ボタンは SoC の GPIO ではなく、PMIC（MAX77620）の EN0 につながっている。
- **読み方:** ドライバーは、I2C5 の MAX77620（0x3C）を 50 ms ごとに読む。
  - ONOFFSTAT（0x15）の bit 2: 押している間は 1。
  - ONOFFIRQ（0x0B）: 読むと消える。50 ms より短い押し方も、ここで拾う。
- **Windows への渡し方:** HID の「System Power Down」ボタンとして報告する。押したときに何をするかは、Windows の電源オプションの「電源ボタンを押したときの動作」で決まる。
- **長押し:** 数秒押し続けると、これまでどおり PMIC が自分で電源を切る。ドライバーは関係しない。

## 手順

1. **Switch を UEFI R374 で起動する。** ペイロードは `D:\Codex\coreboot-R374.rom`。
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\PowerButton`。
3. **先に、ボタンを押したときの動作を「画面を消す」にする。** この Switch は、スリープ（S3 やモダンスタンバイ）がまだ動くかわからない。そのため最初は、安全な「画面を消す」で試す。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\PowerButton\PowerButton.ps1 -Mode SetAction -Action Display
   ```
4. **インストールする。**
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\PowerButton\PowerButton.ps1 -Mode Install
   ```
5. **電源ボタンを短く押す。** 画面が消えれば成功。タッチするか、もう一度ボタンを押すと、画面が戻るはず。
6. **結果を共有する。** `-Mode Status` の出力を見せてほしい。
   - 押しても何も起きないとき: `-Mode Watch` を実行して、ボタンを押したときに `presses` と `reports` が増えるか見る。

## 結果（2026-10-25、ユーザー報告）

- **成功した。** 動作を「画面を消す」にして電源ボタンを押すと、画面が消えて、ロックされた。もう一度押すと、画面がついた。

## ほかの動作にしたいとき

- 押したらシャットダウンする:
  ```bat
  powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\PowerButton\PowerButton.ps1 -Mode SetAction -Action Shutdown
  ```
- 何もしない: `-Action Nothing`
- スリープ: `-Action Sleep`（スリープが動くか確かめるまでは、使わないでほしい）
