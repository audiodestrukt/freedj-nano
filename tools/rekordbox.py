#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""rekordbox.py: read a rekordbox USB export's analysis files (PIONEER/USBANLZ).

    tools/rekordbox.py /run/media/$USER/STICK            # list the analysed tracks
    tools/rekordbox.py /run/media/$USER/STICK --dump N   # one track's sections

Used by pack_tracks.py (--rekordbox) for the beat grid and the CDJ-3000's 3-band waveform.
Format: Deep Symmetry's crate-digger, src/main/kaitai/rekordbox_anlz.ksy. Big-endian; a
"PMAI" file header, then sections: fourcc, u32 header length, u32 section length, body.
  PPTH  path of the audio file on the stick (UTF-16BE)            .DAT
  PQTZ  beat grid: u32, u32, u32 count, then (u16 beat-in-bar,
        u16 BPM x 100, u32 ms) per beat                          .DAT
  PWV7  3-band scrolling waveform: u32 entry bytes (3), u32
        entries, u32, then entries; 150 per second                .2EX
"""
import argparse, os, struct, sys

WAVE_PER_SEC = 150   # PWV7 columns per second (as the monochrome detail waveform)


def sections(path):
    """{fourcc: body bytes} of one ANLZ file (the last section of each kind wins)."""
    out = {}
    try:
        data = open(path, "rb").read()
    except OSError:
        return out
    if data[:4] != b"PMAI":
        return out
    pos = struct.unpack(">I", data[4:8])[0]
    while pos + 12 <= len(data):
        tag = data[pos:pos + 4].decode("latin-1")
        hlen, slen = struct.unpack(">II", data[pos + 4:pos + 12])
        if slen < 12: break
        out[tag] = data[pos + 12:pos + slen]
        pos += slen
    return out


def analysis(anlz_dat):
    """One track from its ANLZ0000.DAT (and the .2EX beside it): path, grid, 3-band wave."""
    s = sections(anlz_dat)
    t = {"anlz": anlz_dat}
    if "PPTH" in s:
        n = struct.unpack(">I", s["PPTH"][:4])[0]
        t["path"] = s["PPTH"][4:4 + n - 2].decode("utf-16-be")
    if "PQTZ" in s:
        b = s["PQTZ"]
        count = struct.unpack(">I", b[8:12])[0]
        t["grid"] = [struct.unpack(">HHI", b[12 + 8 * i:20 + 8 * i]) for i in range(count)]   # (beat, bpm*100, ms)
    x = sections(anlz_dat[:-4] + ".2EX")
    if "PWV7" in x:
        b = x["PWV7"]
        eb, n = struct.unpack(">II", b[:8])
        t["wave3"] = (eb, b[12:12 + eb * n])
    return t


def tracks(root):
    """Every analysed track on the stick, keyed by its audio path relative to the stick."""
    out = {}
    for d, _, files in os.walk(os.path.join(root, "PIONEER", "USBANLZ")):
        if "ANLZ0000.DAT" in files:
            t = analysis(os.path.join(d, "ANLZ0000.DAT"))
            if "path" in t:
                out[t["path"].lstrip("/")] = t
    return out


def find(root, audio_file):
    """The analysis for an audio file on the stick, or None."""
    rel = os.path.relpath(os.path.abspath(audio_file), os.path.abspath(root))
    return tracks(root).get(rel)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root")
    ap.add_argument("--dump", type=int)
    a = ap.parse_args()
    ts = sorted(tracks(a.root).items())
    if a.dump is None:
        for i, (p, t) in enumerate(ts):
            g = t.get("grid") or []
            bpm = f"{g[0][1] / 100:.2f}" if g else "-"
            print(f"{i:3d}  {bpm:>7} BPM  {'3band' if 'wave3' in t else '     '}  {p}")
        print(f"{len(ts)} tracks", file=sys.stderr)
        return
    p, t = ts[a.dump]
    print(p)
    g = t.get("grid") or []
    print(f"grid: {len(g)} beats, first {g[:3]}, BPMs {sorted(set(b[1] for b in g))[:8]}")
    if "wave3" in t:
        eb, w = t["wave3"]
        print(f"PWV7: {len(w) // eb} entries x {eb} bytes = {len(w) // eb / WAVE_PER_SEC:.1f} s at {WAVE_PER_SEC}/s; "
              f"max per byte {[max(w[i::eb]) for i in range(eb)]}")


if __name__ == "__main__":
    main()
