#!/bin/bash
# AArch64 variant of run.sh: compiles the REAL VirtualRealTimeClockLib.c with the firmware's own compiler flags
# (-Os -funsigned-char -mstrict-align -mgeneral-regs-only ...) plus -O0/-O2 for good measure, runs it bare metal in
# qemu-system-aarch64 (-M virt) and checks every result against Python's datetime with the same check.py.
# Needs aarch64-linux-gnu-gcc/-objcopy, qemu-system-aarch64, python3. Slow (minutes): ~207k timestamps per run.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PKG=$(cd "$HERE/../.." && pwd)
BM=$HERE/baremetal
OUT=${OUT_DIR:-$(mktemp -d)}
CC=${CROSS_COMPILE:-aarch64-linux-gnu-}gcc
rc=0
t() {   # <__DATE__ string> <expected YYYY-MM-DD> <opt>
  local tag; tag=$(echo "$1-$3" | tr -c 'A-Za-z0-9-\n' _)
  $CC $3 -g -fshort-wchar -fno-builtin -fno-strict-aliasing -Wall -Werror -funsigned-char -ffreestanding -nostdlib \
      -mlittle-endian -fno-short-enums -fno-pic -fno-pie -mcmodel=small -ffixed-x18 -mstrict-align -mgeneral-regs-only \
      -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections \
      -Wno-builtin-macro-redefined -D__DATE__="\"$1\"" -I "$BM/shim" \
      -T "$BM/link.ld" "$BM/start.S" "$BM/main.c" "$PKG/Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.c" \
      -o "$OUT/rtc_$tag.elf"
  printf '#!/bin/bash\nRTC_BM_ELF=%q exec python3 %q\n' "$OUT/rtc_$tag.elf" "$BM/qemu-run.py" > "$OUT/run_$tag.sh"
  chmod +x "$OUT/run_$tag.sh"
  printf '%-4s ' "$3"
  python3 "$HERE/check.py" "$OUT/run_$tag.sh" "$1" "$2" || rc=1
}
for opt in -Os -O0 -O2; do
  t "Oct  5 2026" 2026-10-05 $opt
  t "Feb 29 2028" 2028-02-29 $opt
  t "Mar  1 2100" 2100-03-01 $opt
  t "Xyz 99 1999" 2018-06-01 $opt
done
exit $rc
