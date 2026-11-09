#!/usr/bin/env bash
# Build optional AArch64 EfiFs v1.13 drivers as standalone EFI files.
# Usage: Tools/build-efifs-extras.sh <edk2-workspace> <output-directory> [toolchain]
set -euo pipefail

edk2=${1:?Usage: build-efifs-extras.sh <edk2-workspace> <output-directory> [toolchain]}
out=${2:?Usage: build-efifs-extras.sh <edk2-workspace> <output-directory> [toolchain]}
toolchain=${3:-GCC}
package="$edk2/EfiFsPkg/EfiFsPkg.dsc"
build_root="$edk2/Build/EfiFs/RELEASE_${toolchain}/AARCH64"

if [[ ! -f "$package" ]]; then
  echo "EfiFs package not found: $package (run Tools/setup_efifs.sh first)" >&2
  exit 1
fi
mkdir -p "$out"

# The 25 drivers embedded by SWITCH_FSDRV are omitted. These are the remaining
# upstream v1.13 formats; keep them standalone to avoid growing BL33/FV.
drivers=(Afs Bfs CpioBe Minix MinixBe Minix2 Minix2Be Minix3 Minix3Be ProcFs UfsBe)

for driver in "${drivers[@]}"; do
  inf="EfiFsPkg/EfiFsPkg/${driver}.inf"
  output="$out/${driver,,}_aa64.efi"
  if [[ -e "$output" ]]; then
    echo "Refusing to overwrite existing output: $output" >&2
    exit 1
  fi
  build -a AARCH64 -b RELEASE -t "$toolchain" -p "$package" -m "$inf"
  built="$build_root/${driver,,}.efi"
  if [[ ! -f "$built" ]]; then
    echo "Expected EDK II output not found: $built" >&2
    exit 1
  fi
  cp -- "$built" "$output"
done

echo "Built ${#drivers[@]} optional read-only drivers in $out"
