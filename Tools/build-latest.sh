#!/bin/bash
# The build that produced release/5/bl33.bin: build-full.sh + FS drivers (needs Tools/setup_efifs.sh <edk2> first),
# 1 MiB FD, and both XHC0 and EHC0 in the DSDT (the firmware selects one at ExitBootServices, see `xusbinit acpi`).
# usage: Tools/build-latest.sh   (run from the directory that contains edk2/ and NintendoSwitchPkg/)
exec "$(dirname "$0")/build-full.sh" -D SWITCH_FSDRV=1 -D SWITCH_BIGFD=1 -D SWITCH_ACPI_XHCI=1 -D SWITCH_ACPI_EHCI=1
