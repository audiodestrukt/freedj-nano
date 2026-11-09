#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""pack_tracks.py: audio files -> one track pack for the `tracks` flash partition.

    tools/pack_tracks.py music/*.mp3 -o tracks.bin            # 22.05 kHz mono (default)
    tools/pack_tracks.py a.wav b.flac --rate 32000 --stereo -o tracks.bin
    tools/pack_tracks.py a.mp3@44100s b.mp3 -o tracks.bin     # per file: @RATE, s stereo / m mono
    esptool --chip esp32c6 write-flash 0x180000 tracks.bin    # offset from firmware/partitions.csv

Decodes anything ffmpeg reads. Format (v2): components/nano/include/nano/library.h.
Also works out, for the CDJ-style screen, a 3-band waveform (low < 200 Hz, mid, high > 2 kHz:
blue, orange, white) and the BPM and first downbeat (onset autocorrelation; --bpm overrides).
The partition is 2.5 MB: about 60 s of 22.05 kHz mono. Tracks that don't fit are left out.
"""
import argparse, os, struct, subprocess, sys
import numpy as np
from scipy import signal

PEAK_MS = 10          # must match ui.cpp's kPeakMs
PART_SIZE = 0x280000  # firmware/partitions.csv


def decode(path, rate, ch):
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "s16le", "-acodec", "pcm_s16le",
                        "-ar", str(rate), "-ac", str(ch), "-"], capture_output=True)
    if r.returncode:
        sys.exit(f"ffmpeg failed on {path}: {r.stderr.decode().strip()}")
    return r.stdout


def mono(pcm, ch):
    a = np.frombuffer(pcm, dtype="<i2").astype(np.float32)
    return a.reshape(-1, ch).mean(axis=1) if ch == 2 else a


def bucket_max(x, n):
    """Max |x| per bucket of n samples."""
    pad = (-len(x)) % n
    return np.abs(np.pad(x, (0, pad))).reshape(-1, n).max(axis=1)


def band_peaks(m, rate):
    """Three bytes per 10 ms bucket: low, mid, high band peaks, each scaled to its own loud end."""
    n = rate * PEAK_MS // 1000
    bands = [signal.sosfilt(signal.butter(4, 200, "lowpass", fs=rate, output="sos"), m),
             signal.sosfilt(signal.butter(2, [200, 2000], "bandpass", fs=rate, output="sos"), m),
             signal.sosfilt(signal.butter(4, 2000, "highpass", fs=rate, output="sos"), m)]
    out = []
    for b in bands:
        p = bucket_max(b, n)
        ref = np.percentile(p, 99) or 1.0
        out.append(np.clip(p / ref * 230, 0, 255).astype(np.uint8))
    return np.stack(out, axis=1).tobytes()     # interleaved low, mid, high


def beat_grid(m, rate, lo=85.0, hi=175.0):
    """(bpm, first downbeat frame) from onset-strength autocorrelation, or (0, 0)."""
    n = rate // 100                               # 10 ms hops
    if len(m) < n * 400: return 0.0, 0            # under 4 s: don't guess
    e = np.log1p(bucket_max(signal.sosfilt(signal.butter(2, 150, "lowpass", fs=rate, output="sos"), m), n)) \
        + 0.5 * np.log1p(bucket_max(m, n))
    on = np.maximum(np.diff(e, prepend=e[0]), 0)
    on -= on.mean()
    ac = np.correlate(on, on, "full")[len(on) - 1:]
    lags = np.arange(len(ac))
    ok = (lags >= 6000 / hi) & (lags <= 6000 / lo)
    if not ok.any(): return 0.0, 0
    k = int(lags[ok][np.argmax(ac[ok])])
    if 1 <= k < len(ac) - 1:                      # parabolic refinement
        a, b, c = ac[k - 1], ac[k], ac[k + 1]
        k = k + 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) else k
    period = float(k)                             # in 10 ms hops
    bpm = 6000.0 / period
    # phase: the offset whose beat comb catches the most onset
    best, phase = -1e9, 0
    for off in range(int(period)):
        idx = np.round(off + np.arange(0, len(on) - off, period)).astype(int)
        sc = on[idx[idx < len(on)]].sum()
        if sc > best: best, phase = sc, off
    return round(bpm, 2), int(phase * n)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--out", default="tracks.bin")
    ap.add_argument("--rate", type=int, default=22050)
    ap.add_argument("--stereo", action="store_true")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=PART_SIZE, help="partition size")
    ap.add_argument("--bpm", type=float, help="force the BPM (all tracks) instead of detecting it")
    a = ap.parse_args()
    ch = 2 if a.stereo else 1

    tracks = []
    used = 8
    for spec in a.files:
        f, rate, fch = spec, a.rate, ch
        if "@" in spec:                                   # file@44100s: rate and channels for this file
            f, opt = spec.rsplit("@", 1)
            fch = 2 if opt.endswith("s") else 1 if opt.endswith("m") else ch
            rate = int(opt.rstrip("sm"))
        pcm = decode(f, rate, fch)
        m = mono(pcm, fch)
        pk = band_peaks(m, rate)
        bpm, first = beat_grid(m, rate)
        if a.bpm: bpm = a.bpm
        need = 64 + len(pcm) + len(pk) + 4
        if used + need > a.size:
            print(f"skip {f}: {need // 1024} KB doesn't fit ({(a.size - used) // 1024} KB left)")
            continue
        used += need
        name = os.path.splitext(os.path.basename(f))[0][:31]
        tracks.append((name, pcm, pk, rate, fch, bpm, first))
        print(f"{len(tracks):02d} {name}: {rate} Hz {'stereo' if fch == 2 else 'mono'}, "
              f"{len(pcm) // 2 // fch / rate:.1f} s, {len(pcm) // 1024} KB, "
              + (f"{bpm:.2f} BPM, first beat {first / rate:.3f} s" if bpm else "no beat grid"))

    head = bytearray(b"FDJN" + struct.pack("<HH", 2, len(tracks)) + bytes(64 * len(tracks)))
    body = bytearray(head)
    for i, (name, pcm, pk, rate, ch, bpm, first) in enumerate(tracks):
        while len(body) % 4: body.append(0)
        off = len(body); body += pcm
        poff = len(body); body += pk
        entry = name.encode("utf-8")[:31].ljust(32, b"\0") + struct.pack(
            "<IIIIIBB2xII", int(round(bpm * 100)), first, off, len(pcm) // 2 // ch, rate, ch, 1, poff, len(pk) // 3)
        body[8 + 64 * i: 8 + 64 * (i + 1)] = entry
    with open(a.out, "wb") as fh:
        fh.write(body)
    print(f"wrote {a.out}: {len(tracks)} tracks, {len(body) // 1024} of {a.size // 1024} KB")
    print(f"flash: esptool --chip esp32c6 write-flash 0x180000 {a.out}")


if __name__ == "__main__":
    main()
