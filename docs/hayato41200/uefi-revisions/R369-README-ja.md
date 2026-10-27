# R369 — ボタン配列の `_HID` を ACPI0011 に変える（R368 ベース）

## 背景（R368 の結果）

- **ドライバーが付かなかった:** `ACPI\PNP0C40\0` は **problem 28**（`CM_PROB_FAILED_INSTALL`、0xC0000490）だった。
- **標準のドライバー自体はある:** Switch の Windows には `hidinterrupt.inf`（HID Button over Interrupt）と `buttonconverter` が入っている。
- **合う INF が無い:** `findstr "PNP0C40" C:\Windows\INF\*.inf` では、**どの INF にも PNP0C40 が無かった**。

**[推測]** Windows 10 の `hidinterrupt.inf` が受け持つのは、ACPI 6.0 の Generic Buttons Device（**`ACPI0011`**）。R368 で使った `_DSD` の UUID `fa6bd625-...` も、iasl が「Generic Buttons Device」と注釈するとおり、ACPI0011 のためのもの。PNP0C40 は Windows 8 の古い ID。

## 変更点

- **`Dsdt-R369.asl`:** `BTNS` の `_HID` を `"PNP0C40"` から **`"ACPI0011"`** に変えた。GpioInt（190 / 191、Edge、ActiveBoth）と `_DSD` は R368 のまま。
- **UEFI:** 表示を R369 にした。ボタンの状態の行も `R369 buttons:` になる。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R369.rom`
- SHA-256: `0940baf1f8b1cc24cd8811aad9b774adfb7784f437101b744d81f7dbdfa7f7bf`
- `EDK_SOURCE_RESTORE_MATCH=YES`。iasl の警告は、もとからある 3128 の1件だけ。
- AML を逆アセンブルして確認した: `_HID "ACPI0011"`、`"\\_SB.GPI0"`、ピン 0x00BE と 0x00BF、`_DSD` の UUID。
- **未確認:** 実機で `hidinterrupt` が付くか。

## 結果（2026-10-25、ユーザー報告）: 段階2は完了

**[事実]** R369 と GPIO ドライバー v3 の組み合わせで、**音量＋と音量−が Windows で正しく反応した。** 押しっぱなし（長押し）でも問題なかった。
- 標準の `hidinterrupt`（ACPI0011）が自動で付いた。
- GPIO の割り込み（バンク 5、GSI 119、ActiveBoth）が正しく動いていることも確認できた。

UEFI では押すと 1 と表示されたが、Windows の側では押した・離したの扱いに問題は出なかった。

## 試験の手順

1. **R369 で起動する。** GPIO ドライバー v3 は、インストール済みのままでよい。
2. **ボタンを押してみる。** 音量＋と音量−を押して、音量の表示（OSD）が出るか見る。
3. **状態を確かめる。** 次を実行し、`button device:` と `driver trace:` の行を見せてほしい。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Stage1\Stage1-Gpio.ps1 -Mode Status
   ```
