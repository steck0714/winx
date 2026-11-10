#!/bin/sh
# ARM64 cross-build against KMDF 1.15. Pass the WindowsDrivers tree and an output directory.
set -eu
src=${1:?WindowsDrivers source directory}
out=${2:?output directory}
: "${WDK_INCLUDE:?WDK Include/version}"
: "${WDK_LIB:?WDK Lib/version/km/ARM64}"
: "${WDF_INCLUDE:?WDK Include/wdf/kmdf/1.15}"
: "${WDF_LIB:?WDK Lib/wdf/kmdf/ARM64/1.15}"
extra=${WDK_EXTRA_LIB:-$WDK_LIB}
cc=${CLANG_CL:-clang-cl}; ld=${LLD_LINK:-lld-link}
for pair in Tegra210Gpio:tegra210gpio Tegra210I2c:tegra210i2c SwitchLiteTouch:switchtouch SwitchPowerButton:switchpwrbtn SwitchFan:switchfan SwitchBattery:switchbattery SwitchRtc:switchrtc SwitchBluetooth:switchbt SwitchSensor:switchsio TegraXusb:tegraxusb; do
    dir=${pair%:*}; name=${pair#*:}; dest="$out/$name"
    driver_dir="$src/drivers/$dir"
    mkdir -p "$dest"
    for file in "$driver_dir/src/$name.c" "$src/tools/kmdf_support.c"; do
        obj=${file##*/}; obj=${obj%.c}.obj
        "$cc" --target=aarch64-pc-windows-msvc /nologo /c /kernel /GS- /Gy /Zl /O2 /W4 /WX \
            -Wno-unused-parameter -Wno-microsoft-anon-tag -Wno-ignored-pragma-intrinsic \
            -Wno-pragma-pack -Wno-invalid-token-paste -Wno-unknown-pragmas -Wno-ignored-attributes \
            -Wno-nonportable-include-path /D_ARM64_ /DARM64 /D_WIN64 /DNTDDI_VERSION=0x0A000008 \
            /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=15 \
            /DPOOL_NX_OPTIN=1 /DSWITCH_LITE_TOUCH=1 "/imsvc$WDK_INCLUDE/km/crt" "/imsvc$WDK_INCLUDE/km" \
            "/imsvc$WDK_INCLUDE/shared" "/imsvc$WDK_INCLUDE/km/spb/1.0" "/imsvc$WDF_INCLUDE" \
            "/Fo$dest/$obj" -- "$file"
    done
    case "$dir" in
        Tegra210Gpio) set -- "$extra/msgpioclxstub.lib" ;;
        Tegra210I2c)  set -- "$extra/spb/1.0/spbcxstubs.lib" ;;
        SwitchFan|SwitchRtc|SwitchBluetooth|TegraXusb) set -- ;;
        SwitchBattery) set -- "$extra/battc.lib" ;;
        SwitchLiteTouch|SwitchPowerButton|SwitchSensor)  set -- "$extra/vhfkm.lib" "$extra/libcntpr.lib" ;;
    esac
    "$ld" /nologo /driver /subsystem:native,10.0 /machine:arm64 /entry:FxDriverEntry /nodefaultlib \
        /release /opt:ref /opt:icf /tsaware:no /section:.text,ERP /section:.rdata,RP /section:.data,RWP \
        /section:.pdata,RP /section:INIT,ERD /timestamp:0 "/out:$dest/$name.sys" "/map:$dest/$name.map" \
        "$dest/$name.obj" "$dest/kmdf_support.obj" "$WDF_LIB/wdfdriverentry.lib" "$WDF_LIB/wdfldr.lib" \
        "$WDK_LIB/ntoskrnl.lib" "$WDK_LIB/hal.lib" "$WDK_LIB/ntstrsafe.lib" "$extra/bufferoverflowfastfailk.lib" "$@"
done
