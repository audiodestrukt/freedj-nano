#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""fw/prolink.c beside Deckbench's simulated players, as freedj-3000's tools/deckbench/suite.py tests OpenDeck.

Deckbench (github.com/commit-collective/deckbench) runs players 2 and 3 on the loopback (127.0.0.2, .3; 2 playing
at 126 BPM, tempo master). sim/prolink_seg runs beside them at 127.0.0.1, claims a number (AUTO), plays at 120 BPM,
SYNC at 8 s, MASTER at 20 s. Every check reads the bench's own view of us, as another player sees us:

  claim    AUTO hears players 2 and 3 and claims 4
  seen     the bench lists player 4 at 127.0.0.1, with its status
  follow   we follow player 2 as tempo master at 126.00 BPM (our own log)
  sync     after SYNC (from 0.37 beat off) we report 126 BPM, synced, our beats within 0.05 beat of the master's
  master   after MASTER we are tempo master and player 2 has let go

    python3 sim/prolink_bench.py [--deckbench ~/sandbox/thirdparty/deckbench] [--keep DIR]
Loopback only: it takes MASTER. Exit status 0 = every check passed.
"""
import argparse, json, os, re, subprocess, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ME, PLAYER, MASTER_TEMPO = "127.0.0.1", 4, 126.0
# The number claim takes ~5.7 s first. Our deck starts 0.37 beat off and SYNC (fw/linksync.c) closes at most
# 3 % of a beat per master beat, as freedj-3000's: ~15 beats, so the phase is read 10 s after SYNC.
SYNC_AT, MASTER_AT, END = 8.0, 20.0, 30.0
LOOK_SYNC, LOOK_MASTER = 18.0, 26.0


def beat_at(phase, t):
    t0, beats, bpm = phase
    return beats + (t - t0) * bpm / 60.0


def device(state):
    for d in (state or {}).get("protocols", {}).get("prolink", {}).get("devices", []):
        if d.get("number") == PLAYER and d.get("ip") == ME:
            return d
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deckbench", default=os.path.expanduser("~/sandbox/thirdparty/deckbench"))
    ap.add_argument("--seg", default=str(HERE.parent / "build/prolink_seg"))
    ap.add_argument("--keep")
    a = ap.parse_args()
    if not os.path.isdir(a.deckbench):
        print(f"prolink_bench: no Deckbench at {a.deckbench}, skipped")
        return 0
    work = Path(a.keep or tempfile.mkdtemp(prefix="prolink-bench-")).resolve()
    work.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, a.deckbench)
    from deckbench.testing import Bench
    snaps = {}
    with Bench(numbers=[2, 3], on=True, init={2: "play,master"}, shift=0) as bench:
        bench.post(f"api/deck/2/tempo/{MASTER_TEMPO:g}")
        log = open(work / "prolink_seg.log", "w")
        seg = subprocess.Popen([a.seg, "lo", str(END), "--auto", "--play", "120",
                                "--sync-at", str(SYNC_AT), "--master-at", str(MASTER_AT)], stdout=log, stderr=subprocess.STDOUT)
        t0 = time.time()
        try:
            while seg.poll() is None:
                el = time.time() - t0
                s = bench.state()
                for mark in (LOOK_SYNC, LOOK_MASTER):
                    if el >= mark and mark not in snaps:
                        snaps[mark] = s
                if device(s):
                    snaps["last"] = s
                time.sleep(0.25)
        finally:
            if seg.poll() is None:
                seg.kill()
    json.dump({str(k): v for k, v in snaps.items()}, open(work / "bench-states.json", "w"), indent=1, default=str)
    out = (work / "prolink_seg.log").read_text()
    results = []

    def check(name, ok, detail):
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL':5s} {name:7s} {detail}", flush=True)

    claimed = re.search(r"ours: player (\d+)", out)
    check("claim", claimed is not None and int(claimed.group(1)) == PLAYER,
          f"claimed player {claimed.group(1)}" if claimed else "never claimed a number")
    me = device(snaps.get("last"))
    check("seen", me is not None and bool(me.get("status")), f"player {PLAYER} at {ME}" if me else "the bench never saw us")
    m = re.search(r"master: player 2 @ ([0-9.]+) BPM, playing, beating", out)
    check("follow", m is not None and abs(float(m.group(1)) - MASTER_TEMPO) < 0.01,
          f"followed player 2 at {m.group(1)} BPM" if m else "never followed player 2")
    s = snaps.get(LOOK_SYNC)
    d = device(s)
    if not d:
        check("sync", False, "the bench never saw us")
    else:
        st, deck = d["status"], s["decks"]["2"]
        if not (st.get("sync") and abs(st.get("bpm", 0) - MASTER_TEMPO) < 0.05):
            check("sync", False, f"synced={st.get('sync')} bpm={st.get('bpm')} (master {MASTER_TEMPO})")
        elif not (d.get("phase") and deck.get("phase")):
            check("sync", False, "no beat phase to compare")
        else:
            t = max(d["phase"][0], deck["phase"][0])
            off = (beat_at(d["phase"], t) - beat_at(deck["phase"], t)) % 1.0
            off = min(off, 1.0 - off)
            check("sync", off < 0.05, f"{st['bpm']:.2f} BPM, synced, beats {off:.3f} beat from the master's")
    s = snaps.get(LOOK_MASTER)
    d = device(s)
    if not d:
        check("master", False, "the bench never saw us")
    else:
        mine, theirs = d["status"].get("master"), s["decks"]["2"].get("master")
        check("master", bool(mine) and not theirs, f"we master={mine}, player 2 master={theirs}")
    print(f"prolink_bench: {sum(results)}/{len(results)} passed; logs in {work}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
