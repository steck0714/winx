#!/bin/sh
set -eu
root=${MINGW_SYSROOT:?pass the mingw-w64 header sysroot}
src=$(CDPATH= cd -- "$(dirname "$0")" && pwd -P)
cc=${CLANG:-clang}
link=${LLD_LINK:-lld-link}
export LLVM_DLLTOOL=${LLVM_DLLTOOL:-llvm-dlltool}
mkdir -p ui
"$cc" --target=aarch64-w64-windows-gnu --sysroot="$root" -ffreestanding -fasynchronous-unwind-tables -fno-stack-protector -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -c "$src/touch-check.c" -o ui/touch-check.obj
python3 - <<'PY'
import os, subprocess
from pathlib import Path
libs={
 'kernel32':'CompareStringOrdinal ExitProcess GetModuleHandleW lstrlenW',
 'user32':'wsprintfW GetClientRect ScreenToClient GetPointerInfo InvalidateRect BeginPaint FillRect EndPaint DrawTextW DefWindowProcW KillTimer PostQuitMessage LoadCursorW RegisterClassW GetSystemMetrics CreateWindowExW SetTimer UpdateWindow GetMessageW TranslateMessage DispatchMessageW',
 'gdi32':'TextOutW GetStockObject SetBkMode SelectObject SetTextColor SetDCBrushColor Ellipse CreateFontW DeleteObject',
 'advapi32':'RegOpenKeyExW RegQueryValueExW RegCloseKey',
 'setupapi':'SetupDiGetClassDevsW SetupDiEnumDeviceInfo SetupDiGetDeviceInstanceIdW SetupDiDestroyDeviceInfoList',
 'cfgmgr32':'CM_Get_DevNode_Status'}
for name,s in libs.items():
 d=Path('ui')/(name+'.def');d.write_text('LIBRARY '+name+'.dll\nEXPORTS\n'+'\n'.join(s.split())+'\n')
 subprocess.run([os.environ['LLVM_DLLTOOL'],'-m','arm64','-d',str(d),'-l','ui/'+name+'.lib'],check=True)
PY
"$link" /nologo /machine:arm64 /subsystem:windows,10.0 /release /entry:start /include:_load_config_used /nodefaultlib /opt:ref /timestamp:0 /out:ui/touch-check.exe ui/touch-check.obj ui/*.lib
