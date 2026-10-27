deliver17 (全機能 + SD診断/静止 + 今回の追加)
bl33_fsdrv.bin : NTFS/exFAT(Windows) ext2/3/4/XFS/Btrfs(Linux) F2FS/EROFS/ext4(Android) の読み取り専用FSドライバ入り。FDが1MiBになる(SWITCH_BIGFD)。
bl33_xusbd.bin : FSドライバなし(従来サイズ)。USB mailbox常駐(xusbinit up / daemon / dlog)のみ追加。
efi_drivers/   : FSドライバ単体(.efi)。シェルで `load fs0:\ntfs.efi` → `map -r` でも使える。
winx-bl33.patch: steck0714/winx usually(9eba951)への差分。EfiFsPkgは別途 Tools/setup_efifs.sh <edk2> で用意。
いずれも実機未検証。書き込みは非対応(SDドライバ自体がread-only)。詳細は README.fsdrv。
