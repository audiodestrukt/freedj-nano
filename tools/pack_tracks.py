#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""pack_tracks.py: audio files -> one track pack for the `tracks` flash partition.

    tools/pack_tracks.py music/*.mp3 -o tracks.bin            # 22.05 kHz mono (default)
    tools/pack_tracks.py a.wav b.flac --rate 32000 --stereo -o tracks.bin
    esptool --chip esp32c6 write-flash 0x180000 tracks.bin    # offset from firmware/partitions.csv

Decodes anything ffmpeg reads. Format: components/nano/include/nano/library.h.
The partition is 2.5 MB: about 60 s of 22.05 kHz mono. Tracks that don't fit are left out.
"""
import argparse, os, struct, subprocess, sys

PEAK_MS = 10          # must match ui.cpp's kPeakMs
PART_SIZE = 0x280000  # firmware/partitions.csv


def decode(path, rate, ch):
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "s16le", "-acodec", "pcm_s16le",
                        "-ar", str(rate), "-ac", str(ch), "-"], capture_output=True)
    if r.returncode:
        sys.exit(f"ffmpeg failed on {path}: {r.stderr.decode().strip()}")
    return r.stdout


def peaks(pcm, rate, ch):
    import array
    a = array.array("h"); a.frombytes(pcm)
    bucket = rate * PEAK_MS // 1000 * ch
    out = bytearray()
    for i in range(0, len(a), bucket):
        seg = a[i:i + bucket]
        out.append(min(255, max(max(seg), -min(seg)) >> 7) if seg else 0)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--out", default="tracks.bin")
    ap.add_argument("--rate", type=int, default=22050)
    ap.add_argument("--stereo", action="store_true")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=PART_SIZE, help="partition size")
    a = ap.parse_args()
    ch = 2 if a.stereo else 1

    tracks = []
    used = 8
    for f in a.files:
        pcm = decode(f, a.rate, ch)
        pk = peaks(pcm, a.rate, ch)
        need = 64 + len(pcm) + len(pk) + 4
        if used + need > a.size:
            print(f"skip {f}: {need // 1024} KB doesn't fit ({(a.size - used) // 1024} KB left)")
            continue
        used += need
        name = os.path.splitext(os.path.basename(f))[0][:39]
        tracks.append((name, pcm, pk))
        print(f"{len(tracks):02d} {name}: {len(pcm) // 2 // ch / a.rate:.1f} s, {len(pcm) // 1024} KB")

    head = bytearray(b"FDJN" + struct.pack("<HH", 1, len(tracks)) + bytes(64 * len(tracks)))
    body = bytearray(head)
    for i, (name, pcm, pk) in enumerate(tracks):
        while len(body) % 4: body.append(0)
        off = len(body); body += pcm
        poff = len(body); body += pk
        entry = name.encode("utf-8")[:39].ljust(40, b"\0") + struct.pack(
            "<IIIB3xII", off, len(pcm) // 2 // ch, a.rate, ch, poff, len(pk))
        body[8 + 64 * i: 8 + 64 * (i + 1)] = entry
    with open(a.out, "wb") as fh:
        fh.write(body)
    print(f"wrote {a.out}: {len(tracks)} tracks, {len(body) // 1024} of {a.size // 1024} KB")
    print(f"flash: esptool --chip esp32c6 write-flash 0x180000 {a.out}")


if __name__ == "__main__":
    main()
