#!/bin/bash
# Host-side check of the calendar arithmetic in Library/VirtualRealTimeClockLib (needs gcc + python3).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PKG=$(cd "$HERE/../.." && pwd)
OUT=${OUT_DIR:-$(mktemp -d)}
rc=0
t() {   # <__DATE__ string> <expected YYYY-MM-DD>
  gcc -std=gnu11 -O1 -w -Wno-builtin-macro-redefined -D__DATE__="\"$1\"" -I "$HERE/shim" \
      "$HERE/test_rtc.c" "$PKG/Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.c" -o "$OUT/test_rtc"
  python3 "$HERE/check.py" "$OUT/test_rtc" "$1" "$2" || rc=1
}
t "Oct  5 2026" 2026-10-05
t "Jan  1 2026" 2026-01-01
t "Dec 31 2026" 2026-12-31
t "Feb 29 2028" 2028-02-29     # leap day
t "Mar  1 2100" 2100-03-01     # 2100 is not a leap year
t "Feb 28 2100" 2100-02-28
t "Jan  1 2200" 2200-01-01
t "Xyz 99 1999" 2018-06-01     # unparsable/implausible: documented fallback
exit $rc
