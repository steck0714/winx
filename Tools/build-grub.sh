#!/usr/bin/env bash
# Build an ARM64 EFI GRUB image for optional embedding in the WinX firmware.
# Usage: bash Tools/build-grub.sh <arm64-efi-module-directory> [output.efi]
set -euo pipefail

module_dir=${1:?Usage: bash Tools/build-grub.sh <arm64-efi-module-directory> [output.efi]}
output=${2:-Tools/grubaa64.efi}
mkimage=${GRUB_MKIMAGE:-grub-mkimage}

if ! command -v "$mkimage" >/dev/null 2>&1; then
  echo "GRUB image tool not found: $mkimage" >&2
  exit 1
fi
if [[ ! -d "$module_dir" ]]; then
  echo "ARM64 EFI GRUB module directory not found: $module_dir" >&2
  exit 1
fi
if [[ -e "$output" ]]; then
  echo "Refusing to overwrite existing output: $output" >&2
  exit 1
fi

"$mkimage" -O arm64-efi -d "$module_dir" \
  -p /EFI/WinX --config=Tools/grub/grub-early.cfg -o "$output" \
  part_gpt fat ext2 ntfs chain linux fdt search search_fs_file search_fs_uuid normal configfile

echo "Built $output. Inspect it before enabling SWITCH_GRUB_MENU=1."
