#!/bin/bash
# Host-side check of Drivers/FanDxe against hekate's own fan driver.
#
#   HEKATE_DIR=/path/to/hekate  Tools/test-fan-trace/run.sh
#
# Needs g++ and python3, no hardware and no EDK2. hekate must be a checkout of CTCaer/hekate
# (the sources used are bdk/thermal/fan.c, bdk/power/regulator_5v.c, bdk/soc/gpio.c).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PKG=$(cd "$HERE/../.." && pwd)
: "${HEKATE_DIR:?set HEKATE_DIR to a hekate checkout}"
OUT=${OUT_DIR:-$(mktemp -d)}
CXX="g++ -std=gnu++17 -x c++ -fpermissive -w -O1"

$CXX -I "$HERE/shim/hekate" -I "$HERE/shim/common" -I "$HEKATE_DIR/bdk" \
     "$HERE/main_hekate.cpp" "$HERE/regmodel.cpp" \
     "$HEKATE_DIR/bdk/thermal/fan.c" "$HEKATE_DIR/bdk/power/regulator_5v.c" "$HEKATE_DIR/bdk/soc/gpio.c" \
     -o "$OUT/trace_hekate"

$CXX -I "$HERE/shim/edk2" -I "$HERE/shim/common" -I "$PKG/Include" \
     "$HERE/main_fan.cpp" "$HERE/regmodel.cpp" \
     "$PKG/Drivers/FanDxe/FanDxe.c" "$PKG/Library/GpioLib/GpioLib.c" \
     -o "$OUT/trace_fan"

exec python3 "$HERE/compare.py" "$OUT/trace_hekate" "$OUT/trace_fan"
