#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
out=${1:?output directory}
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -I "$source_dir/Include" \
    "$test_dir/test.c" -o "$out/test-gpu-i2c"
"$out/test-gpu-i2c"
