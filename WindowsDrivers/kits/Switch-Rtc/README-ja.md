# 時計（RTC）：Nintendo Switch の RTC ドライバー

## 中身

| ファイル | 内容 |
|---|---|
| `switchrtc.sys` | KMDF ドライバー（ARM64、テスト署名）。ソースは `..\SwitchRtc\src\switchrtc.c` |
| `switchrtc.inf` | `ACPI\SWOA0801` |
| `Rtc.ps1` | Install、Status、Sync、Remove |

## 仕組み

- **今の問題:** UEFI の時計は仮のもので、起動のたびに決まった日付から始まる。
- **本物の時計:** Switch には MAX77620 の RTC（I2C5 の 0x68）がある。電源を切っても時を刻み続けている。
- **RTC には書き込まない:** Switch 本体の OS（Horizon）は、この RTC の値に自分の「ずれ（オフセット）」を足して時刻にしている。RTC を書き換えると Horizon の時計がずれるので、このドライバーは RTC を読むだけにする。
- **代わりに、Windows 用のずれを覚える。**
  - **時刻を合わせたとき:** Windows が時刻を合わせると（ネットでの同期、手動での変更）、ドライバーが「その時刻 − RTC」をずれとして保存する（`Parameters\OffsetSeconds`）。
  - **起動したとき:** ドライバーが「RTC + 保存したずれ」でシステム時刻を合わせる。2秒以内の差なら何もしない。
- **最初の1回:** ずれがまだ無いので、何もしない。一度ネットで時刻を合わせると、それ以降の起動では正しい時刻から始まる。

## 手順

1. **Switch を UEFI R377 で起動する。** ペイロードは `D:\Codex\coreboot-R377.rom`。
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\Rtc`。
3. **インストールする。**
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Rtc\Rtc.ps1 -Mode Install
   ```
   `RTC (last read by the driver, raw): 20xx-...` が出れば、RTC は読めている。この値は Horizon 用の値なので、今の時刻とずれていてよい。
4. **ネットにつないだ状態で、時刻を合わせる。**
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Rtc\Rtc.ps1 -Mode Sync
   ```
   `learned offset: ... s` が出れば、ずれを覚えた。
5. **ネットを抜いて再起動する。** 起動した直後から時計が正しければ成功。`-Mode Status` の `BootCorrectionSeconds` が、起動時に直した秒数。

## 結果（2026-10-26）

- **v1（1.0.0.1）:** RTC は読めて、ずれも覚えた（`Learns=2`）。ただし、読み方に間違いがあった。
  - `RtcControl=0x2` の bit0 は「1 のとき BCD」だった（Linux の rtc-max77686）。v1 は Hekate の名前（BIN_FORMAT）に合わせて、逆に解釈していた。
  - このため、2進数の値を BCD として読んでいた。生の値（`RtcRaw0=0x020A3523`、`RtcRaw1=0x1A1A0A`）を正しく読むと **2026-10-26 10:35:23**。表示された 2020-10-20 は誤り。
  - RTC には、ほぼ UTC が入っていた（同期した直後の UTC との差は約19分）。
- **v2（1.0.0.2）:** bit0 の意味を直した。v1 で覚えたずれは使わずに捨てる（`OffsetFormat=2` のものだけを使う）。入れ替えたあとに、もう一度 `-Mode Sync` が必要。
- **v2 の実機結果（ユーザー報告）:** 時計が正しく合うようになった。ネットで同期してずれを覚えたあと、再起動しても時刻が合っている。

## 結果を共有してほしいこと

- 手順 3 と 4 の `-Mode Status` の出力。
- 再起動したあとの時計が正しいか。
