#!/bin/sh
# Compare ntk_min.h against the mingw-w64 DDK headers (struct layouts and constants).  See layout_table.inc.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$here/../../build/win}/layout
mkdir -p "$out"
inc=${MINGW_INC:-/usr/share/mingw-w64/include}
if [ ! -f "$inc/ddk/ntddk.h" ]; then
	echo "  SKIP layout check: mingw-w64 DDK headers not found (set MINGW_INC)"
	exit 0
fi
clang --target=aarch64-pc-windows-msvc -O1 -ffreestanding -DLAYOUT_NTK_MIN -I"$here/../include" \
	-c "$here/layout_check.c" -o "$out/ntk_min.o"
clang --target=x86_64-w64-mingw32 -O1 -isystem "$inc" -isystem "$inc/ddk" -c "$here/layout_check.c" -o "$out/reference.o"
python3 -I "$here/layout_cmp.py" "$here/layout_table.inc" "$out/ntk_min.o" "$out/reference.o"
