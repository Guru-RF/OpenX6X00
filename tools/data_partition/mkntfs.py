"""A minimal NTFS signature (boot sector, $MFT and $Volume records) at a byte offset in a file:
enough for libblkid's ntfs prober. Usage: mkntfs.py FILE OFFSET SECTORS"""
import struct
import sys

path, off, sectors = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
spc = 8                       # sectors per cluster
mft_cluster = 4
bs = bytearray(512)
bs[0:3] = b"\xeb\x52\x90"
bs[3:11] = b"NTFS    "
struct.pack_into("<HB", bs, 0x0B, 512, spc)
bs[0x15] = 0xF8
struct.pack_into("<HHI", bs, 0x18, 63, 255, 0)
struct.pack_into("<QQQ", bs, 0x28, sectors - 1, mft_cluster, 2)
bs[0x40] = 0xF6               # MFT record: 2^10 bytes
bs[0x44] = 0x01
struct.pack_into("<Q", bs, 0x48, 0x1234567890ABCDEF)
bs[510:512] = b"\x55\xaa"


def record():
    r = bytearray(1024)
    r[0:4] = b"FILE"
    struct.pack_into("<HHQHHHHII", r, 4, 0x30, 3, 0, 1, 1, 0x38, 1, 0x40, 1024)
    struct.pack_into("<I", r, 0x38, 0xFFFFFFFF)
    return r


with open(path, "r+b") as f:
    f.seek(off)
    f.write(bs)
    mft = off + mft_cluster * spc * 512
    for i in range(4):
        f.seek(mft + i * 1024)
        f.write(record())
