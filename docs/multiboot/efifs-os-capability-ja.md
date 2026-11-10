# EfiFsから分かるOS・ファイルシステム対応

EfiFsはGRUB由来の**読み取り専用ファイルシステムUEFIドライバ**です。OSを識別したり、OSカーネルをロードしたり、Wi-Fi・GPUなどのOSドライバを提供したりするものではありません。下のOS名はファイルシステムとの関係から考えられる候補であり、そのファイルシステムを使う全OSが起動できるという意味ではありません。

25個のEFIバイナリがFVに入っていることはビルド上の事実ですが、全形式の読み込みを実機で確認した意味ではありません。現在、NTFSについては旧Erista WinXの読み取り報告があります。他形式・各形式の境界条件・OS起動は未検証または理論上の候補です。書き込み可能とは扱いません。検証ラベルは[一覧](../verification-status.md)を参照してください。

## WinXに組み込んだ形式

`SWITCH_FSDRV`は、パッチ由来のEfiFs v1.13 AArch64バイナリ25個をファームウェアへ組み込みます。ハッシュ一覧は`efi_drivers/efifs-1.13-aa64/SHA256SUMS`です。

| WinX内 | EfiFs INF | 使われるOS・用途の例 | ファイル参照だけで起動できるか |
| --- | --- | --- | --- |
| はい | NTFS / exFAT | Windows、Windows回復環境、移動用データ | EFIローダーがあり、UEFIから対象ボリュームを見られれば可能。ESPはFAT推奨 |
| はい | ext2、XFS、Btrfs、JFS、ReiserFS、NILFS2 | Linux各種 | カーネル、initramfs、Switch用DTBが別途必要。Btrfsのsubvolume等は個別確認 |
| はい | F2FS、EROFS | Android/Linuxのdata・system image | Android boot imageやブートチェーンは別途必要 |
| はい | SquashFS、ROMFS | Linux live/組み込み | カーネルとrootfs設定が別途必要 |
| はい | HFS、HFS+ | 旧macOSやAppleの一部領域 | APFSではない。iPadOSのシステム領域はこれだけでは読めない |
| はい | UFS1、UFS2、ZFS | BSD/Solaris/Linux系の一部 | UEFIローダーとSwitchのプラットフォーム対応が別途必要 |
| はい | ISO9660、UDF | Linux/BSDのインストールメディア、光学メディア | OSローダー、カーネル対応が別途必要 |
| はい | CBFS、SFS、TAR、CPIO | Coreboot領域、Amiga SFS、アーカイブ/initramfs | ファイル形式対応だけでOS起動はできない |

FAT/FAT32はEDK II標準のFatDxeを使います。WinXで組み込むEfiFsドライバもGRUBモジュールも読み取り専用です。書き込み可能なEFIFSとして扱わないでください。

## 上流EfiFs v1.13の任意追加形式

ローカルに取得したv1.13の`EfiFsPkg` INF一覧から整理しています。FATドライバはEDK II標準FatDxeと重複するため表から省略しています。ファームウェアに追加ビルドされていないドライバは、WinXで自動的に使えるわけではありません。

| INF | 形式 | 想定できるOS／用途例 |
| --- | --- | --- |
| Afs | AFS | Andrew File System |
| Bfs | BeFS | BeOS/Haiku |
| CpioBe | big-endian cpio | initramfs、アーカイブ |
| Minix、Minix2、Minix3と各BE形式 | Minix FS各版 | Minix |
| ProcFs | proc形式 | 仮想ファイルシステム。通常のブート媒体ではない |
| UfsBe | big-endian UFS | 一部のUFSボリューム |

FATはEDK II標準FatDxeと重複するため、EfiFsの任意リストから除外しています。上表の形式を追加する場合は、対象ボリュームで読み込み試験してから使います。

追加形式はLinux/WSL上でEDK II環境を初期化し、`Tools/build-efifs-extras.sh <EDK2-workspace> <空の出力先> [GCC]`を実行すると、個別EFIドライバとしてビルドできます。事前に`Tools/setup_efifs.sh <EDK2-workspace>`を実行し、EDK IIのビルド依存関係を取得して、同じシェルで`edksetup.sh`を読み込みます。スクリプトは内蔵済み25形式とFATを除く11形式をAArch64/Releaseでビルドします。出力先に同名ファイルがあれば上書きせず停止します。生成した`.efi`はESP等にコピーし、UEFI Shellで`load`、`map -r`を行って読み込み専用で確認します。これはWinXの恒久的な内蔵や各ファイルシステム上のOS起動を保証しません。

## OS起動レイヤーの区別

- **Windows ARM64**: NTFS上のWindows Boot ManagerをUEFIアプリとしてチェーンロードします。WinXのWindowsデバイスドライバは別途Windows側で必要です。
- **ARM64 Linux**: GRUBのLinuxローダーからカーネル、initramfs、Switch SKUに合うDTBを渡します。EfiFsはrootファイルの読み込みを補助するだけです。L4T以外もカーネルとDTBがSwitchの電源・割り込み・ストレージに対応すれば、OS別メニューを追加できます。
- **BSD系**: UEFI対応のローダーまたはカーネルが存在し、Switch向けプラットフォーム対応がある場合に限り候補です。UFS対応だけではSwitch上で起動しません。
- **macOS/iPadOS**: APFSはEfiFs v1.13の一覧にありません。HFS+対応をAPFS対応とみなすことはできません。XNU用ローダー、Apple Device Tree、boot_args、カーネル拡張、ストレージ実装が別途必要です。
- **Android/ChromeOS**: ext4/F2FS/EROFSを読めることはAndroid boot image、AVB、vendor boot、init_bootやChromeOS boot flowの実装を意味しません。既存のSwitch向けブートチェーンを使う必要があります。

## 次に追加する際の基準

1. EfiFs上流にその形式のINFが実在することを確認する。
2. ライセンス、AArch64ビルド、読み取り専用性、サイズを確認する。
3. WinXの既存`SWITCH_FSDRV`セットに入れるか、任意ロードのドライバにするかを決める。
4. QEMUの仮想ディスクでファイル一覧と境界条件を確認し、実機では読み取りだけを確認する。
5. OS起動可否は、OSローダー・カーネル・DTB・デバイスドライバの各層を別に判定する。

参照: [EfiFs上流](https://github.com/pbatard/efifs)、[EfiFsの公開ドライバ一覧](https://efi.akeo.ie/)、[EDK II FatPkg](https://github.com/tianocore/edk2/tree/master/FatPkg)
