#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static sanity checks of a freshly linked Windows ARM64 PE (stdlib only).

    verify_pe.py kernel gm20b.sys ntoskrnl.def
    verify_pe.py user   gm20bctl.exe kernel32.def

Exit status 0 = every check passed.  These checks cannot prove that the driver LOADS (nothing here ever ran on
Windows); they catch the usual build mistakes: wrong subsystem, missing WDM flag, bad checksum, imports of anything
but the allowed DLL / functions, sections that are writable AND executable, missing unwind data, bad relocations.
"""
import struct
import sys


def u(b, off, fmt):
    return struct.unpack_from('<' + fmt, b, off)


class PE:
    def __init__(self, data):
        self.b = b = data
        if b[:2] != b'MZ':
            raise SystemExit('not a PE: no MZ')
        self.lfanew = u(b, 0x3c, 'I')[0]
        if b[self.lfanew:self.lfanew + 4] != b'PE\0\0':
            raise SystemExit('not a PE: no PE signature')
        (self.machine, self.nsec, self.timestamp, _, _, self.optsz, self.chars) = u(b, self.lfanew + 4, 'HHIIIHH')
        o = self.opt = self.lfanew + 24
        self.magic = u(b, o, 'H')[0]
        self.entry = u(b, o + 16, 'I')[0]
        self.imagebase = u(b, o + 24, 'Q')[0]
        self.secalign, self.filealign = u(b, o + 32, 'II')
        self.subsysver = u(b, o + 48, 'HH')
        self.sizeofimage, self.sizeofheaders, self.checksum = u(b, o + 56, 'III')
        self.subsystem, self.dllchars = u(b, o + 68, 'HH')
        self.nrva = u(b, o + 108, 'I')[0]
        self.dirs = [u(b, o + 112 + 8 * i, 'II') for i in range(self.nrva)]
        self.chk_off = o + 64
        so = o + self.optsz
        self.sections = []
        for i in range(self.nsec):
            name, vsize, va, rawsz, rawptr, _, _, _, _, ch = struct.unpack_from('<8sIIIIIIHHI', b, so + 40 * i)
            self.sections.append(dict(name=name.rstrip(b'\0').decode('latin1'), vsize=vsize, va=va,
                                      rawsz=rawsz, rawptr=rawptr, ch=ch))

    def off(self, rva):
        for s in self.sections:
            if s['va'] <= rva < s['va'] + max(s['vsize'], s['rawsz']):
                return s['rawptr'] + (rva - s['va'])
        raise ValueError('rva %#x is in no section' % rva)

    def cstr(self, rva):
        o = self.off(rva)
        return self.b[o:self.b.index(b'\0', o)].decode('latin1')

    def imports(self):
        rva, _ = self.dirs[1]
        res = {}
        if not rva:
            return res
        o = self.off(rva)
        while True:
            oft, _, _, name, ft = struct.unpack_from('<IIIII', self.b, o)
            if oft == 0 and name == 0 and ft == 0:
                break
            dll = self.cstr(name).lower()
            t = self.off(oft or ft)
            while True:
                (ent,) = struct.unpack_from('<Q', self.b, t)
                if ent == 0:
                    break
                res.setdefault(dll, []).append('#%d' % (ent & 0xffff) if ent >> 63 else self.cstr((ent & 0x7fffffff) + 2))
                t += 8
            o += 20
        return res

    def reloc_types(self):
        rva, size = self.dirs[5] if self.nrva > 5 else (0, 0)
        types = {}
        if not rva:
            return types
        o, end = self.off(rva), self.off(rva) + size
        while o < end:
            _, blk = struct.unpack_from('<II', self.b, o)
            if blk < 8:
                break
            for i in range((blk - 8) // 2):
                (e,) = struct.unpack_from('<H', self.b, o + 8 + 2 * i)
                types[e >> 12] = types.get(e >> 12, 0) + 1
            o += blk
        return types


def pe_checksum(b, chk_off):
    d = bytearray(b)
    d[chk_off:chk_off + 4] = b'\0\0\0\0'
    if len(d) & 1:
        d.append(0)
    total = sum(struct.unpack('<%dH' % (len(d) // 2), bytes(d)))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (total + len(b)) & 0xffffffff


def def_exports(path):
    names, inexp = set(), False
    for ln in open(path):
        s = ln.split(';')[0].strip()
        if not s:
            continue
        if s.upper() == 'EXPORTS':
            inexp = True
        elif s.upper().startswith('LIBRARY'):
            continue
        elif inexp:
            names.add(s.split()[0])
    return names


def main(argv):
    if len(argv) != 4 or argv[1] not in ('kernel', 'user'):
        raise SystemExit(__doc__)
    mode, path, defpath = argv[1], argv[2], argv[3]
    kernel = mode == 'kernel'
    data = open(path, 'rb').read()
    pe = PE(data)
    bad = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            bad.append(msg)

    print('%s: %d bytes, %d sections, entry rva %#x, image base %#x' % (path, len(data), pe.nsec, pe.entry, pe.imagebase))
    check(pe.machine == 0xAA64, 'machine is ARM64 (0xAA64), got %#x' % pe.machine)
    check(pe.magic == 0x20b, 'PE32+ optional header')
    check(pe.subsystem == (1 if kernel else 3), 'subsystem is %s (%d)' % ('native' if kernel else 'console', pe.subsystem))
    check(bool(pe.chars & 0x0002), 'IMAGE_FILE_EXECUTABLE_IMAGE')
    check(not (pe.chars & 0x0001), 'relocations are not stripped')
    check(bool(pe.dllchars & 0x0040), 'DYNAMIC_BASE (ARM64 images must be relocatable)')
    check(bool(pe.dllchars & 0x0100), 'NX_COMPAT')
    if kernel:
        check(bool(pe.dllchars & 0x2000), 'WDM_DRIVER flag')
    check(pe.secalign >= 0x1000 and pe.sizeofimage % pe.secalign == 0, 'section alignment %#x, SizeOfImage %#x' % (pe.secalign, pe.sizeofimage))
    calc = pe_checksum(data, pe.chk_off)
    if kernel:
        check(pe.checksum != 0 and pe.checksum == calc, 'PE checksum %#x matches the recomputed %#x' % (pe.checksum, calc))
    else:
        check(pe.checksum in (0, calc), 'PE checksum is 0 or correct')

    ex = [s for s in pe.sections if s['ch'] & 0x20000000]
    check(any(s['va'] <= pe.entry < s['va'] + s['vsize'] for s in ex), 'entry point lies inside an executable section')
    for s in pe.sections:
        wx = (s['ch'] & 0x20000000) and (s['ch'] & 0x80000000)
        check(not wx, 'section %-8s %#7x bytes, flags %#010x%s' % (s['name'], s['vsize'], s['ch'], '  (W+X!)' if wx else ''))
        check(s['rawptr'] + s['rawsz'] <= len(data), 'section %s raw data inside the file' % s['name'])
    prev = 0
    for s in pe.sections:
        check(s['va'] >= prev, 'section %s does not overlap its predecessor' % s['name'])
        prev = s['va'] + max(s['vsize'], 1)

    imps = pe.imports()
    allowed = def_exports(defpath)
    want_dll = 'ntoskrnl.exe' if kernel else 'kernel32.dll'
    check(set(imps) <= {want_dll}, 'imports only from %s (found: %s)' % (want_dll, ', '.join(sorted(imps)) or 'nothing'))
    used = set(imps.get(want_dll, []))
    check(used <= allowed, 'every imported function is listed in %s%s' % (defpath, '' if used <= allowed else ': ' + ', '.join(sorted(used - allowed))))
    print('        imports (%d): %s' % (len(used), ' '.join(sorted(used))))
    check(not (pe.nrva > 9 and pe.dirs[9][0]), 'no TLS directory')
    check(not (pe.nrva > 13 and pe.dirs[13][0]), 'no delay-load imports')
    check(not (pe.nrva > 11 and pe.dirs[11][0]), 'no bound imports')
    check(pe.nrva > 3 and pe.dirs[3][1] > 0, 'exception directory (.pdata unwind info) present, %d bytes' % pe.dirs[3][1] if pe.nrva > 3 else '.pdata')
    types = pe.reloc_types()
    check(set(types) <= {0, 10}, 'base relocations are ABSOLUTE/DIR64 only: %s' % types)

    print('PASS' if not bad else 'FAILED: %d check(s)' % len(bad))
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
