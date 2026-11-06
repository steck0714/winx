#!/usr/bin/env python3
"""Derive the R3 per-card build inputs from a read-only, complete SD image."""
import argparse
import os
from pathlib import Path
import stat
import struct
import tempfile
import uuid
import zlib

SECTOR = 512
WINARM_SECTORS = 80 * 1024**3 // SECTOR
BASIC_DATA = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7").bytes_le


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sector_at(image, lba, count=1):
    image.seek(lba * SECTOR)
    data = image.read(count * SECTOR)
    require(len(data) == count * SECTOR, "incomplete sector in the image")
    return data


def c_bytes(data, columns, separator):
    rows = [separator.join(f"0x{b:02x}" for b in data[i:i + columns])
            for i in range(0, len(data), columns)]
    return ",\n        ".join(rows)


def identity(image):
    info = os.fstat(image.fileno())
    require(stat.S_ISREG(info.st_mode), "expected a saved regular image file")
    size = info.st_size
    require(size % SECTOR == 0, "expected 512-byte sectors")
    mbr = sector_at(image, 0)
    gpt = sector_at(image, 1)
    types = mbr[450:510:16]
    require(mbr[510:] == b"\x55\xaa" and types[-1] == 7 and 0xee in types[:3],
            "expected a hybrid MBR with the NTFS partition in slot 4")
    require(types[0] in (0x0b, 0x0c), "expected FAT32 in MBR slot 1")
    fat_start, fat_length = struct.unpack_from("<II", mbr, 446 + 8)
    require(fat_start > 4 and fat_length > 0 and fat_start + fat_length <= size // SECTOR,
            "invalid FAT32 extent")
    require(gpt[:8] == b"EFI PART", "missing primary GPT")
    header_size, check = struct.unpack_from("<II", gpt, 12)
    require(92 <= header_size <= SECTOR, "invalid GPT header length")
    require(zlib.crc32(gpt[:16] + bytes(4) + gpt[20:header_size]) == check,
            "invalid primary GPT header CRC")
    current, backup, first_usable, last_usable = struct.unpack_from("<4Q", gpt, 24)
    entries_lba, count, stride, array_crc = struct.unpack_from("<QIII", gpt, 72)
    require(current == 1 and backup == size // SECTOR - 1 and
            5 <= first_usable <= last_usable < backup, "inconsistent disk geometry")
    require((entries_lba, count, stride) == (2, 11, 128),
            "R3 supports the tested layout with eleven 128-byte GPT entries at LBA 2")
    entries = sector_at(image, entries_lba, 3)[:count * stride]
    require(zlib.crc32(entries) == array_crc, "invalid GPT partition array CRC")
    target = entries[10 * stride:]
    start, end = struct.unpack_from("<QQ", target, 32)
    length = end - start + 1
    require(target[:16] == BASIC_DATA and target[56:].decode("utf-16-le").rstrip("\0") == "WINARM",
            "expected a basic data partition named WINARM in GPT slot 11")
    require(gpt[56:72] != bytes(16) and target[16:32] != bytes(16), "missing disk or partition GUID")
    require(first_usable <= start <= end <= last_usable and end < 0xffffffff and
            length == WINARM_SECTORS, "expected a dedicated 80 GiB partition addressable by SDXC")
    require(struct.unpack_from("<II", mbr, 494 + 8) == (start, length),
            "MBR and GPT disagree about the WINARM extent")
    require(fat_start + fat_length <= start or fat_start > end,
            "FAT32 and WINARM overlap")
    fat_matches = 0
    for offset in range(0, 10 * stride, stride):
        other = entries[offset:offset + stride]
        if other[:16] == bytes(16):
            continue
        lo, hi = struct.unpack_from("<QQ", other, 32)
        require(first_usable <= lo <= hi <= last_usable and (hi < start or lo > end),
                "another GPT entry is invalid or overlaps WINARM")
        if (lo, hi - lo + 1) == (fat_start, fat_length):
            require(other[:16] == BASIC_DATA, "the FAT32 GPT entry must be Microsoft Basic Data")
            fat_matches += 1
        else:
            require(hi < fat_start or lo >= fat_start + fat_length,
                    "another GPT entry overlaps FAT32")
    require(fat_matches == 1, "MBR and GPT disagree about the FAT32 extent")
    for slot in range(3):
        if types[slot] in (0, 0xee):
            continue
        lo, n = struct.unpack_from("<II", mbr, 446 + slot * 16 + 8)
        require(n > 0 and (lo + n <= start or lo > end), "another MBR entry overlaps WINARM")

    boot = sector_at(image, start)
    require(boot[3:11] == b"NTFS    " and boot[11:14] == b"\x00\x02\x08" and
            boot[510:] == b"\x55\xaa", "unsupported NTFS boot sector")
    require(struct.unpack_from("<I", boot, 28)[0] == start and
            struct.unpack_from("<Q", boot, 40)[0] == length - 1, "NTFS and GPT disagree")
    serial = struct.unpack_from("<Q", boot, 72)[0]
    require(serial != 0, "missing NTFS serial")
    return {
        "WINARM_FIRST": str(start), "WINARM_COUNT": str(length),
        "FAT32_FIRST": str(fat_start), "FAT32_COUNT": str(fat_length),
        "WINARM_SERIAL": f"0x{serial:016x}", "LITE_CARD_BYTES": str(size),
        "NTFS_SERIAL_HEX": f"{serial:016x}",
        "WINARM_MBR_SIGNATURE": f"0x{struct.unpack_from('<I', mbr, 440)[0]:08x}",
        "DISK_GUID": c_bytes(gpt[56:72], 16, ","),
        "TYPE_AND_GUID": c_bytes(target[:32], 16, ","),
        "MBR_TAIL": c_bytes(mbr[440:510], 14, ", "),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="complete saved disk image")
    parser.add_argument("output", type=Path, help="new directory for both private build inputs")
    args = parser.parse_args()
    templates = Path(__file__).resolve().parent
    try:
        require(args.image.is_file(), "pass a regular image file")
        with args.image.open("rb") as image:
            fields = identity(image)
        require(not args.output.exists(), "choose a new output directory")
        with tempfile.TemporaryDirectory(dir=args.output.parent) as temp:
            ready = Path(temp) / "target"
            ready.mkdir(mode=0o700)
            for name in ("winarm-target.h", "startnet.cmd"):
                data = (templates / (name + ".in")).read_bytes()
                for key, value in fields.items():
                    data = data.replace(f"@{key}@".encode(), value.encode())
                (ready / name).write_bytes(data)
            ready.rename(args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"configure-target: {error}\n")


if __name__ == "__main__":
    main()
