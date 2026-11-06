#!/bin/sh
# Build the patched WindowsDrivers/drivers/Tegra210Sdhc sources from WinX.
set -eu
src=${1:?pass the Tegra210Sdhc source directory}
out=${2:?pass an output directory}
case ${3:-winarm} in
    winarm) profile=/DNV_T210_LITE_WINARM_WRITE=1 ;;
    read-only) profile=/DNV_T210_LITE_READ_ONLY=1 ;;
    *) echo "Select the winarm or read-only profile" >&2; exit 2 ;;
esac
: "${WDK_INCLUDE:?set to the WDK Include/version directory}"
: "${WDK_LIB:?set to the WDK Lib/version/km/ARM64 directory}"
clang=${CLANG_CL:-clang-cl}
linker=${LLD_LINK:-lld-link}
mkdir -p "$out"
"$clang" --target=aarch64-pc-windows-msvc /c /kernel /GS- /Gy /Zl /O2 /W4 /WX \
    -Wno-unused-parameter -Wno-unused-function -Wno-microsoft-anon-tag \
    -Wno-ignored-pragma-intrinsic -Wno-pragma-pack -Wno-invalid-token-paste \
    -Wno-unknown-pragmas -Wno-ignored-attributes -Wno-nonportable-include-path \
    /D_ARM64_ /DARM64 /D_WIN64 /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00 \
    /DWINVER=0x0A00 /DPOOL_NX_OPTIN=1 "$profile" "/imsvc$WDK_INCLUDE/km/crt" \
    "/imsvc$WDK_INCLUDE/km" "/imsvc$WDK_INCLUDE/shared" "/Fo$out/nvtsdhc.obj" -- "$src/src/nvtsdhc.c"
"$linker" /nologo /driver /subsystem:native,10.0 /machine:arm64 /entry:DriverEntry \
    /nodefaultlib /release /opt:ref /opt:icf /tsaware:no /section:.text,ERP \
    /section:.rdata,RP /section:.data,RWP /section:.pdata,RP /section:INIT,ERD \
    /timestamp:0 "/map:$out/nvtsdhc.map" "/out:$out/nvtsdhc.sys" "$out/nvtsdhc.obj" \
    "$WDK_LIB/ntoskrnl.lib" "$WDK_LIB/hal.lib" "$WDK_LIB/sdport.lib"
