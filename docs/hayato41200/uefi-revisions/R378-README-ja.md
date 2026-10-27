# R378 — 何も挿さずに起動しても、あとから USB を使えるようにする（R377 ベース）

## 前の問題

UEFI は起動したとき、USB-C の口（BM92T）を最大30秒見張っていた。その間に何も挿さないと、「SAFE TIMEOUT」として XUSB の起動ごと飛ばし、BM92T の設定も元に戻していた。そのため、Windows が起動してから USB 機器を挿しても、反応しなかった。

## 変更点

- **何もつながっていなくても、XUSB を起動する:** 起動した XUSB は、そのまま Windows に渡す。このとき、5V（VBUS）は出さず、BQ24193 は充電モードのままにする。BM92T の設定も戻さない。
  - 表示は `R378 no partner after 10 s: XUSB host without VBUS; Windows handles hot-plug.` と、結果の欄の `nothing attached: VBUS off ...`。
- **見張る時間:** 30 秒から 10 秒に短くした。あとから挿したときは Windows が対応するので、長く待つ必要がなくなったため。
- **BQ の watchdog:** 何もつながっていないときも、止めておく。
- **DSDT:** BAT0 に、3つ目の接続として BM92T（I2C1 の 0x18）を足した。これは、バッテリーのドライバー v4 が抜き差しに合わせて役割を切り替えるため。
- **変えていないもの:** 起動時に機器や充電器付きのハブがつながっていたときの動きは、R377 と同じ。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R378.rom`
- SHA-256: `c55a5b9d924710fecd7c42a6e50143b53cee92ee883ec1506a4fa0f0bd02cbdc`
- `EDK_SOURCE_RESTORE_MATCH=YES`。
- AML を逆アセンブルして、BAT0 に 0x36、0x6B、0x18 の3つの接続があることを確認した。
- **実機の結果（2026-10-26、ユーザー報告）:** バッテリーのドライバー v4 と組み合わせて、USB の動作は OK。

## 手順

1. **R378 で、何も挿さずに起動する。**
   - UEFI の画面に `R378 no partner` と出るか見る。
   - Windows まで起動するか見る。
2. **バッテリーのドライバーを v4 に入れ替える。**
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Battery\Battery.ps1 -Mode Install
   ```
   入れ替わらないときは、Switch を再起動する。
3. **Windows で USB 機器を挿す。** キーボードのレシーバーや USB メモリなど。1〜2秒で使えるようになれば成功。
   - 抜くと、`-Mode Status` の `UsbcState` が 0 に戻るはず。
4. **充電器付きのハブも試す。** 挿してから数秒後に、充電とハブの先の機器が使えるようになれば成功。
5. **起動時に機器を挿したままの場合も、今までどおり動くかを見る。**

## もし起動しなくなったら

R377 の ROM で起動すれば、元に戻る。
