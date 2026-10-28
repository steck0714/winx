#!/bin/bash
# Integrated build: every winx switch on (hekate chain, USB, landscape shell, ext, RTC, net, perf, XUSB, Wi-Fi/Bluetooth).
# usage: Tools/build-full.sh   (run from the directory that contains edk2/ and NintendoSwitchPkg/)
set -e
export WORKSPACE=$PWD/edk2 PACKAGES_PATH=$PWD/edk2:$PWD GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
ARGS=("$@"); set --
cd edk2 && . edksetup.sh && make -C BaseTools
build -a AARCH64 -p NintendoSwitchPkg/NintendoSwitch.dsc -t GCC5 -b RELEASE \
  -D SWITCH_HEKATE=1 -D SWITCH_USB=1 -D SWITCH_FB_ROTATE=2 -D SWITCH_EXT=1 -D SWITCH_RTC=1 -D SWITCH_NET=1 \
  -D SWITCH_PERF=1 -D SWITCH_XUSB=1 -D SWITCH_WIFIBT=1 -D SWITCH_EXCFB=1 "${ARGS[@]}"
R=Build/NintendoSwitch-AARCH64/RELEASE_GCC5
python3 ../NintendoSwitchPkg/Tools/make-bl33.py --fd $R/FV/TEGRA210_EFI.fd --map $R/FV/FVMAIN_COMPACT.Fv.map -o bl33.bin
