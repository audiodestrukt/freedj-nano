#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""pack_lite.py: audio files -> TRACKS.BIN for the CH32V003 deck (format: fw/tracks.h).

    ch32v003/tools/pack_lite.py music/*.mp3 -o TRACKS.BIN
    ch32v003/tools/pack_lite.py a.wav --start 30 --length 60 -o TRACKS.BIN

Mono 22.05 kHz G.711 mu-law (lite_ulaw[] decodes it), a 128-column overview per track, a
peak byte per 256 frames for the scrolling waveform,
BPM and first downbeat from tools/pack_tracks.py's detector. Copy TRACKS.BIN to a freshly
formatted FAT32 card (so it's in one piece), or make a card image with sdimage.py.
"""
import argparse, os, re, struct, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import pack_tracks                                    # noqa: E402  (decode, title, beat_grid)

RATE = 22050


def ulaw_table():
    src = open(os.path.join(HERE, "..", "lite", "ulaw.c")).read()
    body = src[src.index("{") + 1: src.index("}")]
    return np.array([int(v) for v in re.findall(r"-?\d+", body)], np.int32)


def ulaw_encode(x, tab):
    """Nearest code in the deck's own decode table."""
    order = np.argsort(tab); st = tab[order]
    x = np.clip(np.round(x), -32768, 32767)
    i = np.clip(np.searchsorted(st, x), 1, len(st) - 1)
    pick = np.where(np.abs(st[i - 1] - x) <= np.abs(st[i] - x), i - 1, i)
    return order[pick].astype(np.uint8)


def overview(m):
    """128 columns of 0..15: peak level per column against the track's loud end."""
    cols = np.array_split(np.abs(m), 128)
    p = np.array([c.max() if len(c) else 0 for c in cols], float)
    ref = p.max() or 1.0
    h = np.clip(np.round(p / ref * 15), 0, 15).astype(np.uint8)
    return bytes(int(h[2 * i]) | int(h[2 * i + 1]) << 4 for i in range(64))


PEAK = 256                                            # fw/tracks.h TRK_PEAK


def peaks(m):
    """One byte per PEAK frames: the loudest |sample| in it, against the track's loudest."""
    pad = (-len(m)) % PEAK
    p = np.abs(np.pad(m, (0, pad))).reshape(-1, PEAK).max(axis=1)
    ref = p.max() or 1.0
    return np.clip(np.round(p / ref * 255), 0, 255).astype(np.uint8).tobytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--out", default="TRACKS.BIN")
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--length", type=float)
    ap.add_argument("--bpm", type=float)
    a = ap.parse_args()
    tab = ulaw_table()
    tracks = []
    for f in a.files:
        m = pack_tracks.mono(pack_tracks.decode(f, RATE, 1, a.start, a.length), 1)
        bpm, first = pack_tracks.beat_grid(m, RATE)
        if a.bpm: bpm = a.bpm
        name = pack_tracks.title(f).encode("ascii", "replace")[:31]
        tracks.append((name, ulaw_encode(m, tab).tobytes(), overview(m), bpm, first, peaks(m)))
        print(f"{len(tracks):02d} {name.decode()}: {len(m) / RATE:.1f} s, {len(m) // 1024} KB"
              + (f", {bpm:.2f} BPM" if bpm else ""))
    head = bytearray(b"FDJL" + struct.pack("<HHII", 1, len(tracks), RATE, 0))
    body = bytearray(head + bytes(128 * len(tracks)))
    for i, (name, mu, ov, bpm, first, pk) in enumerate(tracks):
        body += bytes(-len(body) % 512)
        off = len(body); body += mu
        poff = len(body); body += pk
        e = (name.ljust(32, b"\0") + struct.pack("<IIII", off, len(mu), int(round(bpm * 100)), first) + ov
             + struct.pack("<II", poff, len(pk)) + bytes(8))
        body[16 + 128 * i: 16 + 128 * (i + 1)] = e
    open(a.out, "wb").write(body)
    print(f"wrote {a.out}: {len(tracks)} tracks, {len(body) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
