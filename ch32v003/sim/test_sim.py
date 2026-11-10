#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""The whole CH32V003 deck through the simulator, checked: the firmware (fw/) on an emulated
SD card (FAT32 image), SSD1306, EC11, buttons, pot and UART. Scripted sessions run headless,
faster than real time; the status lines and the audio they write are checked here.

    python3 sim/test_sim.py          (make simtest)
"""
import os, re, subprocess, sys, wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
B = os.path.join(ROOT, "build", "simtest")
SIM = os.path.join(ROOT, "build", "sim")
RATE = 22050
passed = failed = 0


def check(ok, msg):
    global passed, failed
    if ok: passed += 1
    else: failed += 1; print("FAIL", msg)


def write_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(np.clip(x, -32767, 32767).astype("<i2").tobytes())


def make_tracks():
    os.makedirs(B, exist_ok=True)
    t = np.arange(RATE * 20) / RATE
    # "groove": 120 BPM kick over a chord, quiet 8-12 s (a breakdown the overview should show)
    tk = t % 0.5
    g = 9000 * np.exp(-tk * 9) * np.sin(2 * np.pi * 55 * tk)
    for f in (220, 277.2, 329.6): g += 2500 * np.sin(2 * np.pi * f * t)
    g[(t >= 8) & (t < 12)] *= 0.15
    write_wav(f"{B}/groove.wav", g)
    write_wav(f"{B}/tone440.wav", 12000 * np.sin(2 * np.pi * 440 * t[: RATE * 10]))
    run([sys.executable, f"{ROOT}/tools/pack_lite.py", f"{B}/groove.wav", f"{B}/tone440.wav", "-o", f"{B}/TRACKS.BIN"])
    run([sys.executable, f"{ROOT}/tools/sdimage.py", f"{B}/TRACKS.BIN", "-o", f"{B}/sd.img"])
    run([sys.executable, f"{ROOT}/tools/sdimage.py", f"{B}/TRACKS.BIN", "--fragment", "-o", f"{B}/frag.img"])


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: sys.exit(f"{' '.join(cmd)} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def session(name, script, img="sd.img", sim=SIM, extra=()):
    """Run a script; returns ({label: status}, [status...], audio)."""
    p = f"{B}/{name}.txt"
    open(p, "w").write(script)
    out = f"{B}/{name}"
    text = run([sim, "--sd", f"{B}/{img}", "--script", p, "--out", out, *extra])
    rows = []
    for line in text.splitlines():
        m = re.match(r"t=([\d.]+) (\[.*?\]|\S+) (.*)", line)
        if not m: continue
        d = {k: (float(v) if "." in v else int(v)) for k, v in re.findall(r"(\w+)=(-?[\d.]+)", m.group(3))}
        d["t"], d["why"] = float(m.group(1)), m.group(2)
        rows.append(d)
    by = {}
    for r in rows: by.setdefault(r["why"], r)
    with wave.open(f"{out}/sim.wav") as w:
        audio = np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(float)
    return by, rows, audio


def seg(audio, t0, t1):
    return audio[int(t0 * 22059): int(t1 * 22059)]


def freq(x):
    x = x - x.mean()
    z = np.count_nonzero(np.diff(np.signbit(x)))
    return z / 2 / (len(x) / 22059)


def rms(x): return float(np.sqrt(np.mean(x * x))) if len(x) else 0.0


def main():
    if not os.path.exists(SIM): sys.exit("build/sim missing: make build/sim")
    make_tracks()

    # ---- transport, panel
    by, rows, a = session("panel", """
1000 tap play
3000 mark playing
3500 tap cue
4000 mark cued
4200 turn 25
4600 mark searched
5000 press cue
6000 mark preview
6500 release cue
7000 mark released
7500 tap play
8500 tap play
9000 mark paused
""")
    boot = by["boot"]
    check(boot["err"] == 0 and boot["count"] == 2 and boot["track"] == 0, f"boot: {boot}")
    p = by["[mark playing]"]
    check(p["state"] == 1 and abs(p["pos"] - 2.0 * RATE) < 0.05 * RATE, f"playing 2 s: pos {p['pos']}")
    check(rms(seg(a, 1.2, 2.9)) > 2000, "audio while playing")
    check(rms(seg(a, 0.1, 0.9)) < 50, "silence before play")
    c = by["[mark cued]"]
    check(c["state"] == 0 and c["pos"] == 0, f"CUE while playing: back to the cue, paused: {c}")
    check(rms(seg(a, 3.6, 4.0)) < 50, "silence after CUE")
    s = by["[mark searched]"]
    check(s["pos"] == 25 * 4 * 88, f"search 25 detents paused: pos {s['pos']}, want {25 * 4 * 88}")
    pv = by["[mark preview]"]
    check(pv["state"] == 2 and pv["cue"] == 8800 and pv["pos"] > 8800, f"CUE held while paused: preview from the new cue: {pv}")
    check(rms(seg(a, 5.2, 6.4)) > 2000, "audio during the preview")
    r = by["[mark released]"]
    check(r["state"] == 0 and r["pos"] == 8800, f"CUE released: back to the cue: {r}")
    pz = by["[mark paused]"]
    check(pz["state"] == 0 and pz["pos"] > 8800 + 0.9 * RATE, f"PLAY, PLAY: paused further on: {pz}")
    check(all(x["misses"] == 0 for x in rows), "no ring misses")

    # ---- keylock and tempo, measured on a 440 Hz tone (track 2) through everything
    by, rows, a = session("keylock", """
300 key n
600 pot 1012
1000 tap play
3000 mark varispeed
3100 tap push
3200 mark keylock_on
6000 mark keylock
""")
    v = by["[mark varispeed]"]
    check(v["track"] == 1 and abs(v["tempo"] - 98) <= 2, f"pot near the top: tempo +9.8 %: {v}")
    f1 = freq(seg(a, 1.5, 3.0))
    check(abs(f1 / 440 - 1.098) < 0.01, f"varispeed +9.8 %: 440 Hz plays at {f1:.1f}")
    k = by["[mark keylock]"]
    check(k["keylock"] == 1 and k["stretch"] == 1, f"push: keylock on and stretching: {k}")
    f2 = freq(seg(a, 3.6, 6.0))
    check(abs(f2 / 440 - 1) < 0.01, f"keylock +9.8 %: 440 Hz plays at {f2:.1f}")
    moved = k["pos"] - by["[mark keylock_on]"]["pos"]
    check(abs(moved / (2.8 * RATE) - 1.098) < 0.02, f"keylock keeps the tempo: {moved / 2.8 / RATE:.3f}x")

    # ---- list and load
    by, rows, a = session("list", """
500 press shift
600 turn 3
900 mark list
1000 release shift
1100 tap push
1500 mark loaded
1600 press shift
1700 turn -1
1800 release shift
1900 tap push
2300 mark back
""")
    check(by["[mark list]"]["browse"] == 1 and by["[mark list]"]["sel"] == 1, f"SHIFT + turn: list, 2nd row: {by['[mark list]']}")
    check(by["[mark loaded]"]["track"] == 1 and by["[mark loaded]"]["browse"] == 0, f"push loads: {by['[mark loaded]']}")
    check(by["[mark back]"]["track"] == 1, "the first detent only opens the list")

    # ---- the DJ2Go over the bridge: 0x01 + USB-MIDI packets on the USART
    by, rows, a = session("midi", """
500 midi 90 3B 7F
520 midi 80 3B 00
1500 mark play
1600 midi B0 0D 00
1700 mark fader
1800 midi 90 43 7F
1820 midi 80 43 00
1900 mark keylock
2000 midi B0 19 05
2100 mark nudge
2200 midi 90 33 7F
2400 mark cue
2500 key k
2600 mark key
""")
    check(by["[mark play]"]["state"] == 1, "DJ2Go PLAY")
    check(by["[mark fader]"]["tempo"] == 100, f"DJ2Go tempo fader at the bottom: +10 %: {by['[mark fader]']['tempo']}")
    check(by["[mark keylock]"]["keylock"] == 1, "DJ2Go pitch bend -: keylock")
    check(by["[mark nudge]"]["speed"] > 1.1, f"DJ2Go jog while playing nudges: {by['[mark nudge]']['speed']}")
    check(by["[mark cue]"]["state"] == 0 and by["[mark cue]"]["pos"] == 0, "DJ2Go CUE while playing")
    check(by["[mark key]"]["keylock"] == 0, "console key k on the USART")

    # ---- the end of a track
    by, rows, a = session("end", """
300 key n
500 turn 600
5000 tap play
5200 mark playing
7500 mark end
""")
    check(by["[mark playing]"]["state"] == 1, "playing near the end")
    e = by["[mark end]"]
    check(e["state"] == 0 and e["pos"] >= 10 * RATE - 100, f"stops at the end: {e}")

    # ---- cards that don't work
    by, _, _ = session("frag", "500 mark x\n", img="frag.img")
    check(by["boot"]["err"] == 4 and by["boot"]["count"] == 0, f"fragmented TRACKS.BIN refused: err {by['boot']['err']}")
    by, _, _ = session("nocard", "500 mark x\n", img="missing.img")
    check(by["boot"]["err"] == 1, f"no card: err {by['boot']['err']}")

    # ---- SD traffic: the CMD18 stream against single-block CMD17 reads, 5 s of playback
    script = "100 tap play\n5100 mark done\n"
    st, _, _ = session("sd_stream", script)
    s17, _, _ = session("sd_cmd17", script, sim=SIM + "_cmd17")
    per = lambda d: (d["[mark done]"]["sd_bytes"] - d["[press play]"]["sd_bytes"]) / 5.0
    cmds = lambda d: (d["[mark done]"]["sd_cmds"] - d["[press play]"]["sd_cmds"]) / 5.0
    bs, b17 = per(st), per(s17)
    print(f"  SD at 1x: CMD18 stream {bs / 1e3:.1f} KB/s, {cmds(st):.0f} commands/s; "
          f"CMD17 per read {b17 / 1e3:.1f} KB/s, {cmds(s17):.0f} commands/s "
          f"(24 MHz SPI: {bs * 8 / 24e6 * 100:.1f} % vs {b17 * 8 / 24e6 * 100:.1f} % of the bus)")
    check(bs < b17 / 5, "streaming reads move a fraction of the bytes")

    print(f"{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
