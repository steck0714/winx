#!/bin/bash
# Prepares EfiFsPkg (read-only NTFS/exFAT/ext2-4/XFS/Btrfs/F2FS/EROFS UEFI drivers) for `-D SWITCH_FSDRV=1`.
# Usage: Tools/setup_efifs.sh <edk2 dir>      (EfiFs: GPLv3, pbatard/EfiFs v1.12 + GRUB 2.14 sources)
set -e
EDK2=${1:?usage: setup_efifs.sh <edk2 dir>}
W=$(mktemp -d)
git clone --filter=blob:none https://github.com/pbatard/efifs.git "$W/efifs"
(cd "$W/efifs" && git checkout -q v1.12)
mkdir -p "$W/efifs/grub"
# EfiFs v1.12 expects GRUB 2.14 (a git submodule; the tarball is equivalent).
curl -fL -o "$W/grub.tar.xz" http://archive.ubuntu.com/ubuntu/pool/main/g/grub2/grub2_2.14.orig.tar.xz \
  || curl -fL -o "$W/grub.tar.xz" https://ftp.gnu.org/gnu/grub/grub-2.14.tar.xz
tar -xJf "$W/grub.tar.xz" -C "$W/efifs/grub" --strip-components=1
(cd "$W/efifs/grub" && patch -p1 < ../0001-GRUB-fixes.patch)
# Older EDK2 keeps CompilerIntrinsicsLib in ArmPkg; Ubuntu's gcc turns the stack protector on by default.
sed -i 's#MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf#ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf#' "$W/efifs/EfiFsPkg.dsc"
sed -i 's#^  \*_\*_\*_CC_FLAGS                 = -DDISABLE_NEW_DEPRECATED_INTERFACES#& -fno-stack-protector#' "$W/efifs/EfiFsPkg.dsc"
rm -rf "$EDK2/EfiFsPkg"; mv "$W/efifs" "$EDK2/EfiFsPkg"
# (after the move: the script creates an absolute symlink grub/include/grub/cpu)
(cd "$EDK2/EfiFsPkg" && ./set_grub_cpu.sh aarch64)
echo "EfiFsPkg ready in $EDK2/EfiFsPkg (needs libc6-dev-arm64-cross for Btrfs). Build the firmware with -D SWITCH_FSDRV=1 -D SWITCH_BIGFD=1"
