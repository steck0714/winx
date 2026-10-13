#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build / verify bl33.bin for hekate's L4T launcher.

    bl33.bin = 4 KiB entry stub (Tools/Bl33/bl33_stub.S) + TEGRA210_EFI.fd

hekate loads it at 0xAA000000 (BL33_LOAD_BASE) and BL31 jumps there in EL2t.

All checks below are STATIC. They catch layout mistakes (wrong FD base, wrong stub, FD that does
not fit); they do not prove the image boots on a console.

    make-bl33.py --fd TEGRA210_EFI.fd --map FVMAIN_COMPACT.Fv.map -o bl33.bin
    make-bl33.py --verify bl33.bin [--map FVMAIN_COMPACT.Fv.map]
"""
import argparse, hashlib, pathlib, re, shutil, struct, subprocess, sys, tempfile

LOAD_BASE   = 0xAA000000      # hekate: BL33_LOAD_BASE
STUB_SIZE   = 0x1000
FD_BASE     = LOAD_BASE + STUB_SIZE
FD_MAX      = 0x000E0000      # NintendoSwitch.fdf: [FD] Size
REGION_MAX  = 0x00100000      # MemoryMap.h (SWITCH_BOOT_CHAIN_HEKATE): stub+FD region
FDT_MAGIC   = 0xEDFE0DD0      # little-endian view of 0xD00DFEED
STUB_TAG    = b"WINXHKT1"
STUB_SRC    = pathlib.Path(__file__).resolve().parent / "Bl33" / "bl33_stub.S"


def fail(msg):
    sys.exit(f"make-bl33: ERROR: {msg}")


def build_stub(src):
    tools = [shutil.which(f"aarch64-linux-gnu-{n}") for n in ("as", "ld", "objcopy")]
    if not all(tools):
        fail("aarch64-linux-gnu-{as,ld,objcopy} not found (apt install binutils-aarch64-linux-gnu) or pass --stub")
    with tempfile.TemporaryDirectory() as d:
        o, e, b = (pathlib.Path(d, n) for n in ("s.o", "s.elf", "s.bin"))
        subprocess.run([tools[0], "-o", str(o), str(src)], check=True)
        subprocess.run([tools[1], f"-Ttext={LOAD_BASE:#x}", "-o", str(e), str(o)], check=True)
        subprocess.run([tools[2], "-O", "binary", str(e), str(b)], check=True)
        return b.read_bytes()


def check_stub(stub):
    if len(stub) != STUB_SIZE:
        fail(f"stub is {len(stub):#x} bytes, expected {STUB_SIZE:#x}")
    off = struct.unpack_from("<I", stub, 0x10)[0]
    # hekate reads *(u32 *)(LOAD_BASE + off): must stay inside the image and not be the FDT magic.
    if off & 3 or not (0x14 <= off <= STUB_SIZE - 4):
        fail(f"u32 at +0x10 is {off:#x}: hekate would dereference LOAD_BASE+{off:#x} (must be 4-aligned, inside the stub)")
    probe = struct.unpack_from("<I", stub, off)[0]
    if probe == FDT_MAGIC:
        fail("word hekate probes equals the FDT magic: hekate would patch UART strings into the stub")
    if stub[0x20:0x28] != STUB_TAG:
        fail(f"stub tag at +0x20 is {stub[0x20:0x28]!r}, expected {STUB_TAG!r}")
    w0 = struct.unpack_from("<I", stub, 0)[0]
    if w0 >> 26 != 0b000101:
        fail(f"first stub instruction {w0:#010x} is not an unconditional B")
    return off, probe


def b_target(word, pc):
    imm = word & 0x03FFFFFF
    if imm & 0x02000000:
        imm -= 0x04000000
    return pc + imm * 4


def check_stub_branch(stub):
    """Follow the stub: B stub_main ... final B must land on FD_BASE."""
    t0 = b_target(struct.unpack_from("<I", stub, 0)[0], LOAD_BASE)
    pc = t0
    # expected tail: msr daifset,#0xf ; msr spsel,#1 ; isb ; b fd_entry
    exp = [0xD5034FDF, 0xD50041BF, 0xD5033FDF]
    for i, e in enumerate(exp):
        w = struct.unpack_from("<I", stub, pc - LOAD_BASE + 4 * i)[0]
        if w != e:
            fail(f"stub_main+{4*i:#x} is {w:#010x}, expected {e:#010x}")
    wb = struct.unpack_from("<I", stub, pc - LOAD_BASE + 12)[0]
    if wb >> 26 != 0b000101:
        fail("stub_main does not end in an unconditional B")
    t = b_target(wb, pc + 12)
    if t != FD_BASE:
        fail(f"stub branches to {t:#x}, expected FD at {FD_BASE:#x}")
    return t0, t


def parse_map(path):
    txt = pathlib.Path(path).read_text(errors="replace")
    m = re.search(r"\(Fixed Flash Address, BaseAddress=0x([0-9a-fA-F]+), EntryPoint=0x([0-9a-fA-F]+)", txt)
    if not m:
        fail(f"{path}: no 'Fixed Flash Address, BaseAddress=..., EntryPoint=...' line")
    taken = re.search(r"EFI_FV_TAKEN_SIZE\s*=\s*0x([0-9a-fA-F]+)", txt)
    total = re.search(r"EFI_FV_TOTAL_SIZE\s*=\s*0x([0-9a-fA-F]+)", txt)
    used = (int(taken.group(1), 16), int(total.group(1), 16)) if taken and total else None
    return int(m.group(1), 16), int(m.group(2), 16), used


def check_fd(fd, map_path):
    if len(fd) > FD_MAX:
        fail(f"FD is {len(fd):#x} bytes, exceeds PcdFdSize {FD_MAX:#x}")
    if len(fd) < 0x1000:
        fail("FD suspiciously small")
    w = struct.unpack_from("<I", fd, 0)[0]
    if w >> 26 != 0b000101:
        fail(f"FD first instruction {w:#010x} is not B (expected PrePi entry branch)")
    entry = b_target(w, FD_BASE)
    if not (FD_BASE <= entry < FD_BASE + len(fd)):
        fail(f"FD entry branch lands at {entry:#x}, outside the FD [{FD_BASE:#x}, {FD_BASE+len(fd):#x})")
    info = f"FD entry branch -> {entry:#x}"
    if map_path:
        base, ep, used = parse_map(map_path)
        if ep != entry:
            fail(f"map EntryPoint {ep:#x} != FD first-branch target {entry:#x}: FD was not linked for {FD_BASE:#x} "
                 f"(build with -D SWITCH_HEKATE=1)")
        info += f"; map EntryPoint {ep:#x} matches, PrePi image base {base:#x}"
        if used:
            info += f"; FV used {used[0]:#x} of {used[1]:#x} ({used[0] * 100 // used[1]}%)"
    return entry, info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fd", help="TEGRA210_EFI.fd built with -D SWITCH_HEKATE=1")
    ap.add_argument("--map", help="FVMAIN_COMPACT.Fv.map from the same build (cross-checks the link address)")
    ap.add_argument("--stub", help="prebuilt 4 KiB stub (default: assemble Tools/Bl33/bl33_stub.S)")
    ap.add_argument("-o", "--output", help="bl33.bin to write")
    ap.add_argument("--verify", metavar="BL33_BIN", help="only verify an existing bl33.bin")
    a = ap.parse_args()

    if a.verify:
        img = pathlib.Path(a.verify).read_bytes()
        stub, fd = img[:STUB_SIZE], img[STUB_SIZE:]
    else:
        if not (a.fd and a.output):
            ap.error("--fd and -o are required (or use --verify)")
        fd = pathlib.Path(a.fd).read_bytes()
        stub = pathlib.Path(a.stub).read_bytes() if a.stub else build_stub(STUB_SRC)
        img = stub + fd

    off, probe = check_stub(stub)
    t0, tail = check_stub_branch(stub)
    entry, info = check_fd(fd, a.map)
    if len(img) > REGION_MAX:
        fail(f"image {len(img):#x} bytes does not fit the {REGION_MAX:#x}-byte region in MemoryMap.h")

    print(f"stub : {len(stub):#x} B, hekate probe at +0x10 -> +{off:#x} (= {probe:#010x}, not FDT magic), "
          f"entry b -> {t0:#x}, final b -> {tail:#x}")
    print(f"fd   : {len(fd):#x} B file (padded to the FD size); {info}")
    if not a.verify:
        pathlib.Path(a.output).write_bytes(img)
        print(f"wrote {a.output}: {len(img)} B, sha256 {hashlib.sha256(img).hexdigest()}")
    else:
        print("verify OK (static checks only)")
    if not a.map:
        print("note : no --map given; FD link address not cross-checked")


if __name__ == "__main__":
    main()
