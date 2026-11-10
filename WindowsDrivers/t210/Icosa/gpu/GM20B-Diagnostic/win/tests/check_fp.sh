#!/bin/sh
# Kernel code must not touch FP/SIMD registers (the kernel does not save them for us): scan the disassembly.
# usage: check_fp.sh image...
set -e
bad=0
for f in "$@"; do
	n=$(llvm-objdump -d "$f" | grep -cE '(^|[[:space:],\[])(q|d|s|v)[0-9]+([.,[:space:]\]]|$)' || true)
	echo "  $f: $n instruction(s) using FP/SIMD registers"
	[ "$n" = "0" ] || bad=1
done
exit $bad
