# iPadOS/XNUとWinXの起動境界

## 上流の実績とWinXの実績

[ios-on-switch-lmao](https://github.com/brunostjohn/ios-on-switch-lmao)は、iPadOS 26/27をSwitch OLED向けに動かすプロジェクトです。READMEと[動作状況](https://github.com/brunostjohn/ios-on-switch-lmao/blob/main/docs/what-works.mdx)はiPadOSが動作すると報告しています。これは上流プロジェクトのOLED向け実績で、WinXから起動した結果でも、こちらで独立再現した結果でもありません。上流READMEは他のSwitch機種を対象外としています。

上流の[boot README](https://github.com/brunostjohn/ios-on-switch-lmao/blob/main/boot/README.md)と[how-it-works](https://github.com/brunostjohn/ios-on-switch-lmao/blob/main/docs/how-it-works.mdx)が示す経路は、HekateがSwitchrootのBL31とU-Bootを起動し、専用のnative loaderがXNUとApple Device Treeを配置してXNUへ移る構成です。これはWinXのUEFI EFIアプリを`StartImage()`で呼ぶ構成ではありません。

## 上流の固定配置値

次の数値は上流のOLED用BL33パッケージとHekate L4T実装から読み取った配置です。WinXでの物理readbackや、他のSwitch機種での使用実績ではありません。WinX側では**上流由来の理論値**として扱います。

| 上流での値 | 用途 | 出典 |
| --- | --- | --- |
| `0xAA000000` | HekateがBL33/U-Bootパッケージをロードし、BL31が起動するアドレス | [Hekate `l4t.c`](https://github.com/CTCaer/hekate/blob/master/bootloader/l4t/l4t.c) |
| `0xAA070000` | パッケージ内U-Boot script | [上流 boot README](https://github.com/brunostjohn/ios-on-switch-lmao/blob/main/boot/README.md) |
| `0xAA080000` | native XNU loader | 同上 |
| `0xAA0B0000` | SD exporter | 同上 |
| `0xAA0C0000` | Apple Device Tree | 同上 |
| `0xAA100000` | 圧縮済みカーネルのstaging window | [上流 `stage.h`](https://github.com/brunostjohn/ios-on-switch-lmao/blob/main/boot/loader/stage.h) |
| `0x8FE00000` | U-Bootが任意の`bootargs.txt`を置く領域 | [上流 `boot_args.h`](https://github.com/brunostjohn/ios-on-switch-lmao/blob/main/boot/loader/boot_args.h) |

上流の[U-Boot script](https://github.com/brunostjohn/ios-on-switch-lmao/blob/main/boot/u-boot-script.txt)は`t210b01=1`かつ`sku=3`を要求します。Hekateの[L4T起動コード](https://github.com/CTCaer/hekate/blob/master/bootloader/l4t/l4t.c)はBL31向けパラメーター、BL33のEL2 entry、BPMP/DRAM、carveout、ボード別ファームウェアを準備します。これらをWinX UEFIから単純に固定アドレスへロードしてジャンプするだけでは同じ起動条件を作れません。

## 4機種別のiPadOS状況

| SoC / コードネーム | 製品 | iPadOS上流の範囲 | WinXからの起動 |
| --- | --- | --- | --- |
| `t210/Icosa` | 標準Switch / HAC-001 | 上流READMEの対象外 | 未実装・未検証。T210用BL31/メモリ初期化、Apple Device Tree、XNUドライバーの移植が必要。 |
| `t214/Iowa` | バッテリー強化版 / HAC-001-01 | 上流READMEの対象外 | 未実装・未検証。T210B01共通部に加えてIowa固有の電源・表示・入出力設定が必要。 |
| `t214/Hoag` | Switch Lite / HDH-001 | 上流READMEの対象外 | 未実装・未検証。Windows Liteドライバーの結果はXNUドライバーやUEFI起動の証拠ではない。 |
| `t214/Aula` | Switch OLED / HEG-001 | 上流プロジェクトが対象とする機種 | iPadOS上流のHekate経路は別プロジェクトの報告。WinX UEFIからは未実装・未検証。 |

SoC・コードネームの分類はHekateの[hardware-type定義](https://github.com/CTCaer/hekate/blob/master/bdk/soc/fuse.h)に合わせています。`t214`配下の3製品を同一ボードとは扱いません。上流が明記するOLED向け条件を他機種に流用していません。

## WinXへ統合するための未実装項目

WinX UEFIから直接起動するには、少なくとも次の移植が必要です。これは設計チェックリストであり、コードや動作結果ではありません。

1. Hekate L4TのSoC初期化・BL31パラメーター・BPMP/DRAM/MC設定を、WinXの起動状態と競合しない形で置き換える。
2. UEFIから使えるiPadOSローダーを作り、ボード識別、カーネル・trust cache等の配置、Apple Device Tree、`boot_args`を準備する。
3. XNUへの例外レベル、MMU/cache、割り込み、メモリ予約の引き渡しを定義する。Apple XNUの[ARM64起動コード](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm64/start.s)は`boot_args`と初期CPU状態を使いますが、Switch用の具体的なhandoff ABIは別途実装が必要です。
4. APFS上のシステム起動にはXNU側APFS対応が必要です。WinX同梱の[EfiFs一覧](efifs-os-capability-ja.md)にAPFS driverはありません。上流のMac用image builder、Apple IPSWの生成物、Trust Cache等の入手・署名・配置も別工程です。
5. XNUのSD、画面、タッチ、Joy-Con、Wi-Fi、Bluetooth、電源、GPU、音声等を機種別に実装する。Linux/WindowsのドライバーはそのままXNUドライバーにはなりません。
6. 4機種ごとにビルド確認と実機起動・各デバイス試験を行う。現在はOLED以外について上流iPadOS実績もWinX実機結果も記録していません。

現状の安全な複数OS構成案はHekateを最上位メニューとして残し、OLEDでは上流iPadOSのHekate entryとWinX entryを分ける形です。WinX entryからWindows/ARM64 Linuxを選びます。WinX UEFI内にiPadOSを表示して起動できるとするコードはまだありません。GRUBの一般EFI chainloader例も、iPadOSのBL33/U-Boot payloadを起動できるものではありません。

## クレジットとライセンス

- [ios-on-switch-lmao — Bruno St John](https://github.com/brunostjohn/ios-on-switch-lmao): iPadOS/OLED向けのHekate→BL31/U-Boot→native loader→XNU構成を調査するため参照。上流独自コードはGPL-2.0-only。現WinX作業ではソースやバイナリをコピーしていません。
- [Hekate — CTCaer](https://github.com/CTCaer/hekate): L4T BL31/BL33起動とSoC分類の参照。上流コードはGPL-2.0。
- [Switchroot](https://gitlab.com/switchroot): ATF/U-Boot/L4Tの起動基盤に関する参照。各ファイルのライセンスを維持します。
- [Apple XNU](https://github.com/apple-oss-distributions/xnu): `boot_args`とARM64 kernel-entry契約の参照。Appleのソースライセンスを維持します。

WinXの既存EDK II、EfiFs、Hekate、Switchroot等のクレジットは[README](../../README.md#credits)にあります。Apple IPSWやiPadOSシステムイメージはWinXへ同梱しません。
