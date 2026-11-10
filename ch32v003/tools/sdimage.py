#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""sdimage.py: a FAT32 SD card image holding files, for the simulator (sim/).

    ch32v003/tools/sdimage.py TRACKS.BIN -o build/sd.img
    ch32v003/tools/sdimage.py TRACKS.BIN --fragment -o build/frag.img   # file in pieces

Laid out like a card from the factory: an MBR, one FAT32 partition at 4 MB, 32 KB clusters
(so at least 2 GB; the image is sparse). Files go in the root, in consecutive clusters
unless --fragment interleaves them with a filler file. `fsck.fat -n` checks it.
"""
import argparse, os, struct

SECTOR = 512
PART_START = 8192                                  # sectors: 4 MB
RESERVED = 32


def short_name(path):
    base, ext = os.path.splitext(os.path.basename(path).upper())
    return base[:8].ljust(8).encode() + ext[1:4].ljust(3).encode()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--out", default="sd.img")
    ap.add_argument("--cluster-kb", type=int, default=32)
    ap.add_argument("--fragment", action="store_true", help="interleave each file's clusters with a filler file")
    a = ap.parse_args()
    spc = a.cluster_kb * 1024 // SECTOR
    csize = spc * SECTOR
    datas = [open(f, "rb").read() for f in a.files]
    need = sum((len(d) + csize - 1) // csize for d in datas) * (2 if a.fragment else 1) + 16
    clusters = max(need, 65536 + 16)               # FAT32 needs >= 65525
    fatsz = (4 * (clusters + 2) + SECTOR - 1) // SECTOR
    part_sectors = RESERVED + 2 * fatsz + clusters * spc
    total = PART_START + part_sectors

    fat = [0] * (clusters + 2)
    fat[0], fat[1] = 0x0FFFFFF8, 0x0FFFFFFF
    fat[2] = 0x0FFFFFFF                            # root directory: one cluster
    nxt = 3
    entries = [b"FREEDJ     " + bytes([0x08]) + bytes(20)]          # volume label
    placed = []
    filler = []
    for path, d in zip(a.files, datas):
        n = max(1, (len(d) + csize - 1) // csize)
        chain = []
        for k in range(n):
            chain.append(nxt); nxt += 1
            if a.fragment and k < n - 1:
                filler.append(nxt); nxt += 1
        for x, y in zip(chain, chain[1:] + [0x0FFFFFFF]): fat[x] = y
        placed.append((chain, d))
        entries.append(short_name(path) + struct.pack("<BBBHHHHHHHI", 0x20, 0, 0, 0, 0x5821, 0x5821, chain[0] >> 16,
                                                     0, 0x5821, chain[0] & 0xFFFF, len(d)))
    if filler:
        for x, y in zip(filler, filler[1:] + [0x0FFFFFFF]): fat[x] = y
        entries.append(b"FILLER  BIN" + struct.pack("<BBBHHHHHHHI", 0x20, 0, 0, 0, 0x5821, 0x5821, filler[0] >> 16,
                                                   0, 0x5821, filler[0] & 0xFFFF, len(filler) * csize))

    with open(a.out, "wb") as f:
        f.truncate(total * SECTOR)
        def put(sector, data): f.seek(sector * SECTOR); f.write(data)
        # MBR
        mbr = bytearray(SECTOR)
        mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", PART_START, part_sectors)
        mbr[510:512] = b"\x55\xaa"
        put(0, mbr)
        # boot sector
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xeb\x58\x90"; bs[3:11] = b"MSWIN4.1"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, spc, RESERVED, 2, 0, 0, 0xF8, 0, 63, 255, PART_START, part_sectors)
        struct.pack_into("<IHHIHH", bs, 36, fatsz, 0, 0, 2, 1, 6)
        struct.pack_into("<BBBI", bs, 64, 0x80, 0, 0x29, 0x1234ABCD)
        bs[71:82] = b"FREEDJ     "; bs[82:90] = b"FAT32   "; bs[510:512] = b"\x55\xaa"
        put(PART_START, bs); put(PART_START + 6, bs)
        fsi = bytearray(SECTOR)
        struct.pack_into("<I", fsi, 0, 0x41615252); struct.pack_into("<III", fsi, 484, 0x61417272, clusters + 2 - nxt, nxt)
        struct.pack_into("<I", fsi, 508, 0xAA550000)
        put(PART_START + 1, fsi); put(PART_START + 7, fsi)
        fatb = struct.pack(f"<{len(fat)}I", *fat)
        for k in range(2): put(PART_START + RESERVED + k * fatsz, fatb)
        data0 = PART_START + RESERVED + 2 * fatsz
        sect = lambda c: data0 + (c - 2) * spc
        put(sect(2), b"".join(e.ljust(32, b"\0") for e in entries))
        for chain, d in placed:
            for k, c in enumerate(chain): put(sect(c), d[k * csize:(k + 1) * csize])
    print(f"wrote {a.out}: {total * SECTOR / 1e9:.2f} GB (sparse), {clusters} clusters of {a.cluster_kb} KB"
          + (", fragmented" if a.fragment else ""))


if __name__ == "__main__":
    main()
