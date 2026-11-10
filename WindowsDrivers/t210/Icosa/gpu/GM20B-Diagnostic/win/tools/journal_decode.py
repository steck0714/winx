#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode a gm20b journal on the PC (stdlib only).

    journal_decode.py FILE [--tail N] [--explain]   decode C:\\Windows\\gm20b-journal.bin copied from the Switch
    journal_decode.py --hex 0123abcd...             decode the 24-byte registry value
                                                    HKLM\\SYSTEM\\CurrentControlSet\\Services\\gm20b\\LastEntry
                                                    (as printed by `reg query ... /v LastEntry`)
    journal_decode.py --selftest                    check the code tables (unique values, names)

The last record names the operation in flight when the machine hung.  Records whose comment starts with STEP were
made durable BEFORE the hardware access they announce, so a hang right after such a record means that very access
(or the next one) hung the bus.
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SPACES = ['BAR0', 'BAR1', 'CAR', 'PMC', 'MC']
STAGES = ['MAP', 'POWER', 'ID', 'CLK', 'PRIV', 'MEM', 'FIFO', 'ACR', 'GR']
ERRS = ['OK', 'TIMEOUT', 'IO', 'NOMEM', 'NODEV', 'NOTIMPL', 'INVAL', 'FW', 'GATED', 'STATE', 'HW']
ADDR_CODES = {'J_RD', 'J_WR', 'J_WAIT_TIMEOUT', 'J_WAIT_OK', 'J_S2_BOOT0_INTENT', 'J_WIN_BAD_ACCESS', 'J_WIN_RAWRD',
              'J_WIN_RAWRD_DONE', 'J_WIN_RAWWR', 'J_WIN_RAWWR_DONE', 'J_WIN_RAW_DENIED'}
RX = re.compile(r'X\(\s*(J_\w+)\s*,\s*(0x[0-9a-fA-F]+)\s*\)\s*(?:/\*\s*(.*?)\s*\*/)?')


def source_root():
    for cand in (os.path.join(HERE, '..', '..'), HERE):
        if os.path.exists(os.path.join(cand, 'core', 'gm20b_jcodes.h')):
            return cand
    return None


def load_codes():
    """-> ({value: (name, comment)}, [duplicate descriptions])"""
    codes, dups = {}, []
    tsv = os.path.join(HERE, 'jcodes.tsv')
    if os.path.exists(tsv):                      # shipped next to the script in the dist folder
        for ln in open(tsv, encoding='utf-8'):
            v, name, com = (ln.rstrip('\n').split('\t') + ['', ''])[:3]
            codes[int(v, 16)] = (name, com)
        return codes, dups
    root = source_root()
    if not root:
        return codes, dups
    cdir = os.path.join(root, 'core')
    files = [os.path.join(cdir, f) for f in sorted(os.listdir(cdir))
             if (f.startswith('jcodes_') or f.startswith('gm20b_jcodes')) and f.endswith('.h')]
    files.append(os.path.join(root, 'win', 'include', 'gm20b_jcodes_win.h'))
    for p in files:
        for m in RX.finditer(open(p, encoding='utf-8').read()):
            v = int(m.group(2), 16)
            if v in codes:
                dups.append('0x%04x is both %s and %s' % (v, codes[v][0], m.group(1)))
            codes[v] = (m.group(1), m.group(3) or '')
    return codes, dups


def err_name(v):
    v = v - (1 << 32) if v & 0x80000000 else v
    return ERRS[-v] if -len(ERRS) < v <= 0 else 'ERR?'


def fmt(e, codes):
    seq, code, a, b, t = e
    name, com = codes.get(code, ('J_?%04x' % code, ''))
    hint = ''
    if name in ('J_STAGE_BEGIN', 'J_STAGE_OK', 'J_STAGE_SKIP', 'J_FINI_BEGIN', 'J_FINI_OK'):
        hint = STAGES[a] if a < 9 else '?'
    elif name == 'J_STAGE_FAIL':
        hint = '%s: %s' % (STAGES[a] if a < 9 else '?', err_name(b))
    elif name == 'J_INIT_DONE':
        hint = 'stages done %d, %s' % (a, err_name(b))
    elif name == 'J_WIN_INIT_DONE':
        hint = '%s, stages done %d' % (err_name(a), b)
    elif name == 'J_WIN_IOCTL_DONE':
        hint = err_name(b)
    elif name == 'J_S2_BOOT0':
        hint = 'chipset %03x rev %02x' % ((a >> 20) & 0x1ff, a & 0xff)
    elif name in ADDR_CODES and (a >> 28) < 5:
        hint = '%s+%#x' % (SPACES[a >> 28], a & 0x0fffffff)
    step = 'STEP ' if com.startswith('STEP') else '     '
    line = '%08x %9.3f ms  %s%-22s a=%08x b=%08x' % (seq, t / 1e6, step, name, a, b)
    return line + ('  [%s]' % hint if hint else ''), com


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('file', nargs='?')
    ap.add_argument('--hex')
    ap.add_argument('--tail', type=int, default=0, help='print only the last N records')
    ap.add_argument('--explain', action='store_true', help='print the meaning of the last records (default 12)')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--dump-codes', metavar='TSV', help='write jcodes.tsv for the dist folder')
    args = ap.parse_args()
    codes, dups = load_codes()

    if args.selftest:
        bad = list(dups)
        if not codes:
            bad.append('no journal codes found (run from the source tree or put jcodes.tsv next to the script)')
        for v, (n, _) in codes.items():
            if not 0 <= v <= 0xffff:
                bad.append('%s: value out of range' % n)
        names = [n for n, _ in codes.values()]
        bad += ['duplicate name %s' % n for n in set(names) if names.count(n) > 1]
        for b in bad:
            print('FAIL', b)
        print('%d journal codes, %s' % (len(codes), 'PASS' if not bad else 'FAILED'))
        return 1 if bad else 0
    if args.dump_codes:
        with open(args.dump_codes, 'w', encoding='utf-8') as f:
            for v in sorted(codes):
                f.write('%x\t%s\t%s\n' % (v, codes[v][0], codes[v][1]))
        return 0
    if args.hex:
        raw = bytes.fromhex(args.hex.strip().replace(' ', ''))
        if len(raw) != 24:
            raise SystemExit('LastEntry must be 24 bytes (48 hex digits), got %d bytes' % len(raw))
        line, com = fmt(struct.unpack('<IIIIQ', raw), codes)
        print(line)
        if com:
            print('   meaning:', com)
        return 0
    if not args.file:
        ap.print_usage()
        return 2

    data = open(args.file, 'rb').read()
    if len(data) < 32 or struct.unpack_from('<I', data, 0)[0] != 0x4a303247:
        raise SystemExit('%s is not a gm20b journal file (bad magic)' % args.file)
    magic, ver, esz, hsz = struct.unpack_from('<IIII', data, 0)
    if esz != 24:
        raise SystemExit('unsupported entry size %d' % esz)
    n = (len(data) - hsz) // 24
    ents = [struct.unpack_from('<IIIIQ', data, hsz + 24 * i) for i in range(n)]
    if (len(data) - hsz) % 24:
        print('note: %d trailing bytes of a torn record ignored' % ((len(data) - hsz) % 24))
    start = max(0, n - args.tail) if args.tail else 0
    loads = sum(1 for e in ents[:start] if codes.get(e[1], ('',))[0] == 'J_WIN_LOAD')
    for e in ents[start:]:
        if codes.get(e[1], ('',))[0] == 'J_WIN_LOAD':
            loads += 1
            print('=== driver load #%d ===' % loads)
        print(fmt(e, codes)[0])
    print('-- %d records, %d driver load(s)' % (n, sum(1 for e in ents if codes.get(e[1], ('',))[0] == 'J_WIN_LOAD')))
    if n:
        print('\nLAST RECORD: ' + fmt(ents[-1], codes)[0])
        print('If the machine hung, it hung AFTER this record became durable. A STEP record announces the access that\n'
              'follows it; any other record means the hang came from code between it and the next STEP.')
    if args.explain and n:
        print('\nMeaning of the last records:')
        for e in ents[-(args.tail or 12):]:
            line, com = fmt(e, codes)
            print('  %-22s %s' % (codes.get(e[1], ('?',))[0], com or '(no description)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
