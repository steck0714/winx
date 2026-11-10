#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""layout_cmp.py layout_table.inc ntk_min.o reference.o

Extracts the ".lyt" table from two COFF objects and compares them name by name.  Exit 0 = identical."""
import re
import struct
import sys


def lyt(path):
    b = open(path, 'rb').read()
    _, nsec, _, _, _, optsz, _ = struct.unpack_from('<HHIIIHH', b, 0)
    for i in range(nsec):
        name, _, _, rawsz, rawptr = struct.unpack_from('<8sIIII', b, 20 + optsz + 40 * i)
        if name.rstrip(b'\0') == b'.lyt':
            data = b[rawptr:rawptr + rawsz]
            return list(struct.unpack('<%dQ' % (len(data) // 8), data))
    raise SystemExit('%s: no .lyt section' % path)


def main(argv):
    names = re.findall(r'^L\((\w+),', open(argv[1]).read(), re.M)
    a, b = lyt(argv[2]), lyt(argv[3])
    if not (len(names) == len(a) == len(b)):
        print('table sizes differ: %d names, %d / %d values' % (len(names), len(a), len(b)))
        return 1
    bad = 0
    for n, x, y in zip(names, a, b):
        if x != y:
            bad += 1
            print('  MISMATCH %-34s ntk_min.h = %#x   reference DDK headers = %#x' % (n, x, y))
    print('  %d facts compared, %s' % (len(names), 'all identical' if not bad else '%d MISMATCH(ES)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
