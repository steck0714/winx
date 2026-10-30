#!/bin/bash
# Windows-for-Switch (hayato41200) stack on the hekate boot chain.
# usage: Tools/build-woa.sh   (run from the directory that contains edk2/ and NintendoSwitchPkg/)
# Needs: edk2-stable202402 with patches/edk2/*.patch applied, and
#        NintendoSwitchPkg/Firmware/tegra21x_xusb_firmware (see Firmware/README.md).
set -e
export WORKSPACE=$PWD/edk2 PACKAGES_PATH=$PWD/edk2:$PWD GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
ARGS=("$@"); set --
[ -f NintendoSwitchPkg/Firmware/tegra21x_xusb_firmware ] || { echo "missing Firmware/tegra21x_xusb_firmware"; exit 1; }
for p in NintendoSwitchPkg/patches/edk2/*.patch; do
  (cd edk2 && patch -p1 -N --binary --dry-run < ../$p >/dev/null 2>&1 && patch -p1 -N --binary < ../$p) || true
done
cd edk2 && . edksetup.sh && make -C BaseTools
build -a AARCH64 -p NintendoSwitchPkg/NintendoSwitch.dsc -t GCC5 -b RELEASE \
  -D SWITCH_HEKATE=1 -D SWITCH_WOA=1 -D SWITCH_BIGFD=1 -D SWITCH_FB_ROTATE=2 "${ARGS[@]}"
R=Build/NintendoSwitch-AARCH64/RELEASE_GCC5
python3 ../NintendoSwitchPkg/Tools/make-bl33.py --fd $R/FV/TEGRA210_EFI.fd --map $R/FV/FVMAIN_COMPACT.Fv.map -o bl33.bin
