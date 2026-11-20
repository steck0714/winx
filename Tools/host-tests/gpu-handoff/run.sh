#!/bin/sh
# Host test for GpuHandoff.h (no EDK II needed).
set -e
cd "$(dirname "$0")"
cc -std=gnu11 -Wall -Wextra -Werror -O2 test.c -o /tmp/gpu-handoff-test
/tmp/gpu-handoff-test
