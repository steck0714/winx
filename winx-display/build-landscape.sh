#!/bin/sh
set -eu
root=${MINGW_SYSROOT:?mingw-w64 header sysroot}
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?output directory}
cc=${CLANG:-clang}
link=${LLD_LINK:-lld-link}
export LLVM_DLLTOOL=${LLVM_DLLTOOL:-llvm-dlltool}
mkdir -p "$out"
"$cc" --target=aarch64-w64-windows-gnu --sysroot="$root" -ffreestanding \
    -fasynchronous-unwind-tables -fno-stack-protector -O2 -Wall -Wextra -Werror \
    -c "$src/landscape.c" -o "$out/landscape.obj"
python3 - "$out" <<'PY'
import os, subprocess, sys
from pathlib import Path
out=Path(sys.argv[1])
libs={
    'kernel32':'ExitProcess GetLastError',
    'user32':'EnumDisplaySettingsExW ChangeDisplaySettingsExW',
    'advapi32':'RegCreateKeyExW RegOpenKeyExW RegQueryValueExW RegSetValueExW RegCloseKey'}
for name, symbols in libs.items():
    d=out/(name+'.def')
    d.write_text('LIBRARY '+name+'.dll\nEXPORTS\n'+'\n'.join(symbols.split())+'\n')
    subprocess.run([os.environ['LLVM_DLLTOOL'],'-m','arm64','-d',str(d),'-l',str(out/(name+'.lib'))],check=True)
PY
"$link" /nologo /machine:arm64 /subsystem:windows,10.0 /release /entry:start \
    /include:_load_config_used /nodefaultlib /opt:ref /timestamp:0 \
    "/out:$out/LiteLandscape.exe" "$out/landscape.obj" \
    "$out/kernel32.lib" "$out/user32.lib" "$out/advapi32.lib"
