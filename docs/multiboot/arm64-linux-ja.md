# WinXからのARM64 Linux複数起動

WinXのUEFIは共通のファームウェアとして使い、OS選択は後段のGRUBに任せます。通常のUEFI初期化でLinux向けの電源設定やACPI変更を実行しません。

**これは設計・ソース調査上の起動案です。GRUB生成、現在のWinXイメージへの組み込み、Switch実機からのLinux起動はいずれも未確認です。** 要求値やDTB資源値は対象カーネルとソースに基づく理論値として扱ってください。

```text
Hekate → BL31 → WinX BL33 / EDK II → GRUB ARM64 EFI → Windows / Linux
```

LinuxはGRUBの標準Linuxローダーで起動します。WindowsはARM64のWindows Boot Managerへチェーンロードします。GRUBの`linux`、`initrd`、`devicetree`、`chainloader`はOS別のエントリで動作するため、Linuxを起動しないときはWinX上の別OSにDTBを差し替えません。

## Linuxの条件

- AArch64対応のLinuxカーネルと、GRUBから読める未圧縮`Image`。
- 起動するSwitchの基板に一致するDTB。Icosa (Erista/T210)、Iowa (Mariko/T210B01)、Hoag (Lite/T210B01)、Aula (OLED/T210B01)では同じDTBを使えるとは限りません。
- ルートファイルシステムに必要なストレージ/ファイルシステムドライバーが、カーネルまたはinitramfsに入っていること。
- 各ディストリビューション固有の`root=`, `rootfstype=`, `rootflags=`と、必要なinitramfs。

GRUBの`devicetree`コマンドはファイルからDTBを読み、ファームウェアのツリーとマージせず置き換えます。したがってDTBは該当カーネル・Switch SKUに合わせ、必要なoverlay適用済みのものを用意してください。AArch64 Linux boot protocolはDTBを必須とし、GRUBのLinuxローダーはそれをカーネルへ渡します。[Linux ARM64 boot protocol](https://docs.kernel.org/arch/arm64/booting.html) · [Linux EFI stub](https://docs.kernel.org/admin-guide/efi-stub.html) · [GRUB devicetree](https://www.gnu.org/software/grub/manual/grub/html_node/devicetree.html)

## Switchroot L4Tとの違い

Switchrootの標準起動経路はHekateのL4T LoaderからATF、U-Bootへ進み、U-Bootがカーネル、initramfs、DTBを用意します。[Switchroot bootstack](https://wiki.switchroot.org/wiki/linux/linux-bootstack-documentation)。GRUB起動はこのU-Boot処理を通らないため、`boot.scr`だけをGRUBから起動するものではありません。ディストリビューションが使う最終カーネル、DTB、initramfs、カーネル引数を個別エントリに設定します。L4T以外のSwitch向けARM64ディストリビューションも、同じ条件を満たせばエントリを追加できます。

標準のPC ARM64ディストリビューションがSwitchのSoC/デバイスツリーに対応していない場合、起動ローダーだけでは対応できません。Linuxカーネル、DTB、initramfsにSwitchのTegraデバイス対応が必要です。

## ESPとGRUB設定

1. WinX BL33/UEFIは保持し、ESPに`\EFI\WinX\grub.cfg`を用意します。GRUBは`Tools/build-grub.sh`で生成し、`-D SWITCH_HEKATE=1 -D SWITCH_BIGFD=1 -D SWITCH_GRUB_MENU=1`を付けてWinXをビルドするとファームウェア内に組み込まれます。
2. `grub.cfg.example`を`\EFI\WinX\grub.cfg`へコピーし、ESP UUID、Linux root PARTUUID、DTB、kernel、initramfsを実機の配置に合わせます。複数Linuxはエントリを複製し、各エントリでDTBとrootを分けます。
3. GRUB EFIバイナリには少なくとも`part_gpt`, `fat`, `ext2`, `ntfs`, `chain`, `linux`, `fdt`, `search`, `search_fs_uuid`, `search_fs_file`, `normal`モジュールを含めます。GRUB自身が読むファイルシステム用モジュールです。WinXのEfiFsはUEFIシェルから非FATボリュームを読む機能を追加しますが、GRUBは独自のファイルシステムモジュールを使います。
4. `SWITCH_GRUB_MENU`ビルドはGRUBをUEFIの先頭起動項目として登録します。HekateからWinXを選択し、GRUBのメニューにWindowsとLinuxの項目が出ることを確認します。Linuxエントリの初回試験ではログを有効にし、失敗時に元のHekate/BL33エントリから復旧できることを確認します。

GRUB 2系をLinuxホストでビルドする例です。ディストリビューションごとにAArch64 EFIターゲットのモジュール配置が異なるため、`-d`はそのホストのARM64 EFIモジュールディレクトリに合わせます。

```sh
grub-mkimage -O arm64-efi -d /usr/lib/grub/arm64-efi \
  -p /EFI/WinX --config=Tools/grub/grub-early.cfg -o Tools/grubaa64.efi \
  part_gpt fat ext2 ntfs chain linux fdt search search_fs_uuid search_fs_file normal
```

`Tools/build-grub.sh`は同じ生成手順を使い、既存の出力ファイルを上書きしません。Secure BootのlockdownでGRUBの`devicetree`が禁止される構成では、Switch向けに署名鍵を信頼させる設計が別途必要です。

## 実装状況

このリポジトリにはSKU別Linux DTB、Linuxカーネル/initramfsは含まれません。GRUB組み込みと任意ビルド経路を追加しましたが、GRUBバイナリの生成、WinXファームウェアのビルド、Switch実機でのLinux起動は未検証です。手元にあるSwitchrootの`boot.scr`やDTBを無条件でコピーして使うことは避け、対象SKU・カーネルに対応するものを選びます。
