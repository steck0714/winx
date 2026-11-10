#!/bin/sh
# Build the shared upstream PortCls/WaveRT audio driver for ARM64.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?output directory}
: "${WDK_INCLUDE:?WDK Include/version}"
: "${WDK_LIB:?WDK Lib/version/km/ARM64}"
extra=${WDK_EXTRA_LIB:-$WDK_LIB}
cc=${CLANG_CL:-clang-cl}
ld=${LLD_LINK:-lld-link}
mkdir -p "$out"
"$cc" --target=aarch64-pc-windows-msvc /nologo /c /kernel /GS- /Gy /Zl /O2 /W4 /GR- \
    -Wno-nonportable-include-path -Wno-pragma-pack -Wno-microsoft-anon-tag \
    -Wno-ignored-pragma-intrinsic -Wno-unknown-pragmas -Wno-ignored-attributes \
    -Wno-invalid-token-paste /D_ARM64_ /DARM64 /D_WIN64 /D_KERNEL_MODE \
    /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00 /DPOOL_NX_OPTIN=1 \
    "/imsvc$WDK_INCLUDE/km/crt" "/imsvc$WDK_INCLUDE/km" \
    "/imsvc$WDK_INCLUDE/shared" "/imsvc$WDK_INCLUDE/km/spb/1.0" \
    "/Fo$out/switchaudio.obj" -- "$src/src/switchaudio.cpp"
"$ld" /nologo /driver /subsystem:native,10.0 /machine:arm64 /entry:DriverEntry /nodefaultlib \
    /release /opt:ref /opt:icf /tsaware:no /timestamp:0 \
    /section:.text,ERP /section:.rdata,RP /section:.data,RWP /section:.pdata,RP /section:INIT,ERD \
    "/out:$out/switchaudio.sys" "/map:$out/switchaudio.map" \
    "$out/switchaudio.obj" "$extra/portcls.lib" "$WDK_LIB/ntoskrnl.lib" \
    "$WDK_LIB/hal.lib" "$WDK_LIB/ntstrsafe.lib" "$extra/libcntpr.lib" "$extra/bufferoverflowfastfailk.lib"
