#!/bin/sh
# Cross-build the upstream display-only driver for Windows 11 ARM64.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/winx-display" && pwd)
out=${1:?output directory}
: "${WDK_INCLUDE:?WDK Include/version}"
: "${WDK_LIB:?WDK Lib/version/km/ARM64}"
extra=${WDK_EXTRA_LIB:-$WDK_LIB}
cc=${CLANG_CL:-clang-cl}
ld=${LLD_LINK:-lld-link}
mkdir -p "$out"
for unit in bdd bdd_ddi bdd_dmm bdd_util bltfuncs blthw memory; do
    "$cc" --target=aarch64-pc-windows-msvc /nologo /c /kernel /GS- /Gy /Zl /O2 /W3 /GR- \
        /Zc:threadSafeInit- -Wno-nonportable-include-path -Wno-pragma-pack \
        -Wno-microsoft-anon-tag -Wno-ignored-pragma-intrinsic -Wno-unknown-pragmas \
        -Wno-ignored-attributes -Wno-invalid-token-paste \
        /D_ARM64_ /DARM64 /D_WIN64 /D_KERNEL_MODE /DUNICODE /D_UNICODE \
        /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00 \
        /DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM2_0 \
        /DD3D_UMD_INTERFACE_VERSION=D3D_UMD_INTERFACE_VERSION_WDDM2_0 \
        "/imsvc$WDK_INCLUDE/km/crt" "/imsvc$WDK_INCLUDE/km" \
        "/imsvc$WDK_INCLUDE/shared" "/imsvc$WDK_INCLUDE/um" \
        "/Fo$out/$unit.obj" -- "$src/$unit.cxx"
done
"$ld" /nologo /driver /subsystem:native,10.0 /machine:arm64 /entry:DriverEntry /nodefaultlib \
    /release /opt:ref /opt:icf /tsaware:no /timestamp:0 \
    /section:.text,ERP /section:.rdata,RP /section:.data,RWP /section:.pdata,RP /section:INIT,ERD \
    "/out:$out/SampleDisplay.sys" "/map:$out/SampleDisplay.map" \
    "$out/bdd.obj" "$out/bdd_ddi.obj" "$out/bdd_dmm.obj" "$out/bdd_util.obj" \
    "$out/bltfuncs.obj" "$out/blthw.obj" "$out/memory.obj" \
    "$extra/displib.lib" "$WDK_LIB/ntoskrnl.lib" "$WDK_LIB/hal.lib" \
    "$WDK_LIB/ntstrsafe.lib" "$extra/libcntpr.lib" "$extra/bufferoverflowfastfailk.lib"
