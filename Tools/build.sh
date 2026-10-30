#!/bin/bash
# One entry point for winx.
# usage: Tools/build.sh [woa|ehci] [extra build args, e.g. -D SWITCH_FSDRV=1]
#   woa   (default) hekate boot chain + Windows-for-Switch stack (UsbPowerAuditDxe + XhciDxe + SWOA* ACPI devices).
#                   Needs Firmware/tegra21x_xusb_firmware (see Firmware/README.md).
#   ehci            the previous stack (EhciDxeTegra + PciEmulation, XHC0/EHC0 selectable via UBSL): Tools/build-latest.sh.
# env:
#   SD=nvtsdhc      describe SDM1 for the Windows-for-Switch nvtsdhc driver (Windows 11 22621+). Leave unset for Windows 10.
#                   Do not combine with SDLOADAA64*.EFI: both would describe the SD host.
# Run from the directory that contains edk2/ and NintendoSwitchPkg/ (same as the scripts below).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
profile=woa
case "${1:-}" in woa|ehci) profile=$1; shift;; esac
extra=()
[ "${SD:-}" = "nvtsdhc" ] && extra+=(-D SWITCH_ACPI_SD_NVTSDHC=1)
case "$profile" in
  woa)  exec "$here/build-woa.sh" "${extra[@]}" "$@" ;;
  ehci) exec "$here/build-latest.sh" "${extra[@]}" "$@" ;;
esac
