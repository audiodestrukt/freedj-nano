#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""fm1host.py: the computer as the FM-1 deck's SD card, over USB (the FREEDJ USB module, fm1/usb/).

    python3 tools/fm1host.py build/fm1/usb.img [--port "(emu)"]

The module asks for blocks with MIDI CCs on channel 16 (102..105 the block, 7 bits each from the
low end; 106 how many from it for the run, 108 as a one-off sent first; 107 hello) and gets them as fm1-dock MODULE frames (SysEx
F0 7D 48 42 23 seq pack7(payload + CRC-32) F7, each answered by fm1-dock before the next):
  'A' n + n x (u32 first, u32 end)   the mu-law's block ranges (a missing block there is silence)
  'P' u32                            where the next blocks go: the pinned set
  512 bytes                          a block (the pinned set)
  'H' u32 block, u8 half, 256 bytes  half a block, addressed (the stream; each frame then 32 events of
                                     filler, CC 112 on channel 16, so fm1-dock has taken it before the next)
  'G'                                the pinned set is complete: the deck starts
On hello it sends the ranges and every block that isn't track data (the FAT, the directory,
TRACKS.BIN's header and entries), then 'G'; after that, the blocks asked for, in order (a request
that carries on from the current run extends it; any other starts a new run).

The image: tools/sdimage.py's (sparse, read by its data regions). Needs python-rtmidi.
In fm1-emu: fm1-live --usb makes the FM-1 a MIDI device here, "<product> (emu)".
"""
import argparse, collections, os, struct, sys, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memcard import sectors  # noqa: E402

MAN = bytes([0x7D, 0x48, 0x42])
MODULE = 0x23
PIN_MAX = 48
WINDOW = 16                                     # frames in flight (an answer takes ~50 ms to come back through fm1-live: 4 capped it at ~40 blocks a second)
# fm1-dock holds one SysEx frame until its main loop takes it, and drops one that starts arriving before: after
# each frame, 32 MIDI events of nothing (CC 112 on channel 16, which the module ignores: 2 USB packets). A frame
# still lost now and then costs little: the module asks again. More filler is traffic too: fm1-dock takes about
# one packet a millisecond in fm1-emu, and at 96 the stream fell just short of what the deck plays
FILLER = int(os.environ.get("FM1HOST_FILLER", "32"))
LOST_S = 0.5                                    # no answer to a frame in flight for this long: its slot is freed


class Lost(Exception):
    """frames went unanswered: stream again from block .n (None: not a block)"""
    def __init__(self, n):
        self.n = n
DEBUG = os.environ.get("FM1HOST_DEBUG") == "1"   # every message from the FM-1


def pack7(b):
    out, acc, nb = bytearray(), 0, 0
    for x in b:
        acc |= x << nb; nb += 8
        while nb >= 7: out.append(acc & 0x7F); acc >>= 7; nb -= 7
    if nb: out.append(acc & 0x7F)
    return bytes(out)


def unpack7(b):
    out, acc, nb = bytearray(), 0, 0
    for x in b:
        acc |= (x & 0x7F) << nb; nb += 7
        if nb >= 8: out.append(acc & 0xFF); acc >>= 8; nb -= 8
    return bytes(out)


def frame(seq, payload):
    body = payload + struct.pack("<I", zlib.crc32(payload))
    return bytes([0xF0]) + MAN + bytes([MODULE, seq & 0x7F]) + pack7(body) + bytes([0xF7])


def reply(msg):
    """(seq, status) of an fm1-dock answer to a MODULE frame, else None"""
    if len(msg) < 7 or msg[0] != 0xF0 or msg[-1] != 0xF7 or bytes(msg[1:4]) != MAN or msg[4] & ~0x40 != MODULE:
        return None
    d = unpack7(msg[6:-1])
    if len(d) < 5 or zlib.crc32(d[:-4]) != struct.unpack_from("<I", d, len(d) - 4)[0]:
        return None
    return msg[5], d[0]


class Card:
    """the image's blocks, and which of them are track data"""
    def __init__(self, image):
        keep, size = sectors(image)
        self.blk = dict(keep)
        self.blocks = size // 512
        tl = next((n for n, b in keep if b[:4] == b"FDJL"), None)
        if tl is None:
            raise SystemExit(f"{image}: no TRACKS.BIN in it")

        def file(off, n):
            out = bytearray()
            while len(out) < n:
                b = self.blk.get(tl + (off + len(out)) // 512, bytes(512))
                at = (off + len(out)) % 512
                out += b[at:at + n - len(out)]
            return bytes(out)
        count = struct.unpack_from("<H", file(0, 16), 6)[0]
        self.audio, self.stream = [], []
        for i in range(count):
            e = file(16 + 128 * i, 128)
            off, frames = struct.unpack_from("<II", e, 32)
            poff, plen = struct.unpack_from("<II", e, 112)
            self.audio.append((tl + off // 512, tl + (off + frames) // 512))
            end = max(off + frames, poff + plen)
            self.stream.append((tl + off // 512, tl + (end + 511) // 512))
        self.pinned = [n for n, _ in keep if not any(a <= n < b for a, b in self.stream)]
        print(f"{image}: {count} tracks, {len(keep)} blocks with data, {len(self.pinned)} pinned "
              f"(the rest streamed: {sum(b - a for a, b in self.stream)} blocks)")
        if len(self.pinned) > PIN_MAX:
            raise SystemExit(f"{len(self.pinned)} pinned blocks: the module keeps {PIN_MAX}")

    def get(self, n):
        return self.blk.get(n, bytes(512))


class Host:
    def __init__(self, card, port):
        import rtmidi
        self.card = card
        self.out, self.inp = rtmidi.MidiOut(), rtmidi.MidiIn()
        end = time.monotonic() + 60
        while True:                             # wait for it (fm1-live takes a few seconds to enumerate);
            names, ins = self.out.get_ports(), self.inp.get_ports()   # several: the newest, listed last
            o = [i for i, n in enumerate(names) if port.lower() in n.lower()]
            i_ = [i for i, n in enumerate(ins) if port.lower() in n.lower()]
            if o and i_:
                break
            if time.monotonic() > end:
                raise SystemExit(f"no MIDI port with {port!r} in its name: {names}")
            time.sleep(0.5)
        if len(o) > 1:
            print(f"{len(o)} ports match {port!r}: taking the newest (close the other FM-1 windows)")
        self.out.open_port(o[-1])
        ins = self.inp.get_ports()              # again: opening the output adds a port of ours to this list
        i_ = [i for i, n in enumerate(ins) if port.lower() in n.lower()]
        self.inp.open_port(i_[-1])
        self.inp.ignore_types(sysex=False, timing=True, active_sense=True)
        print(f"on {names[o[-1]]}")
        self.seq, self.cc, self.job, self.place, self.oneoffs = 0, {}, None, None, []
        self.passes = self.misses = self.cpu = 0
        self.flight = collections.deque()      # (seq, when, block or None) sent, not yet answered
        self.sent, self.t0, self.busy, self.frames, self.losses = 0, time.monotonic(), 0.0, 0, 0

    def poll(self):
        """MIDI from the module: requests (kept), and fm1-dock's answers (returned)"""
        answers = []
        while True:
            m = self.inp.get_message()
            if not m:
                return answers
            msg = m[0]
            if DEBUG:
                print("in", " ".join(f"{b:02x}" for b in msg[:12]))
            if msg[0] == 0xBF and len(msg) == 3:
                self.cc[msg[1]] = msg[2]
                if msg[1] in (106, 108):
                    lba = sum(self.cc.get(102 + i, 0) << (7 * i) for i in range(4))
                    if msg[1] == 106:
                        self.want(lba, lba + msg[2])
                    elif all(lba != a for a, _ in self.oneoffs):
                        self.oneoffs.append((lba, lba + msg[2]))
                elif msg[1] == 107:
                    self.job = ("boot",)
                elif msg[1] == 113:
                    self.cpu = msg[2]
                elif msg[1] == 111:
                    self.passes = self.cc.get(109, 0) | self.cc.get(110, 0) << 7
                    self.misses = msg[2]
            elif msg[0] == 0xF0:
                r = reply(msg)
                if r: answers.append(r)

    def want(self, a, b):
        j = self.job
        if j and j[0] == "run" and j[1] <= a <= j[2]:
            self.job = ("run", j[1], max(j[2], b))
        elif not (j and j[0] == "boot"):
            self.job = ("run", a, b)

    def send(self, payload):
        t = time.monotonic()
        try:
            return self._send(payload)
        finally:
            self.busy += time.monotonic() - t; self.frames += 1

    def _send(self, payload):
        for _ in range(8):
            self.seq = (self.seq + 1) & 0x7F
            self.out.send_message(list(frame(self.seq, payload)))
            end = time.monotonic() + 0.5
            while time.monotonic() < end:
                for seq, st in self.poll():
                    if seq == self.seq:
                        if st:
                            print(f"the module said {st} to a {len(payload)}-byte frame")
                        return st == 0
                time.sleep(0.0003)
        raise SystemExit("no answer from fm1-dock: is FREEDJ USB the module playing?")

    def boot(self):
        c = self.card
        rng = c.audio[:63]
        self.send(b"A" + bytes([len(rng)]) + b"".join(struct.pack("<II", a, b) for a, b in rng))
        last = None
        for n in c.pinned:
            if n != last:
                self.send(b"P" + struct.pack("<I", n))
            self.send(c.get(n))
            last = n + 1
        self.send(b"G")
        self.place = None
        print(f"pinned set sent ({len(c.pinned)} blocks): the deck starts")

    # Streaming: frames go without waiting for each answer, up to WINDOW in flight (the answers are the flow
    # control: fm1-dock holds one frame at a time). Nothing is resent from here: the module sees which blocks
    # never came and asks for them again.
    def stream(self, payload, blk):
        while len(self.flight) >= WINDOW:
            self.drain()
            time.sleep(0.0003)
        self.seq = (self.seq + 1) & 0x7F
        self.out.send_message(list(frame(self.seq, payload)))
        self.flight.append((self.seq, time.monotonic(), blk))

    def drain(self):
        for seq, st in self.poll():
            while self.flight and self.flight[0][0] != seq and any(s == seq for s, _, _ in self.flight):
                self.flight.popleft()           # (one before it went unanswered: lost; the module asks again)
                self.losses += 1
            if self.flight and self.flight[0][0] == seq:
                _, _, b = self.flight.popleft()
                if st:
                    print(f"the module said {st}")
                elif b is not None:
                    self.sent += 1
        while self.flight and time.monotonic() - self.flight[0][1] > LOST_S:
            self.flight.popleft()               # no answer: its slot back (the module asks again for what's missing)
            self.losses += 1

    def settle(self):
        """until every frame in flight is answered (before the stop-and-wait boot)"""
        end = time.monotonic() + 1.0
        while self.flight and time.monotonic() < end:
            try:
                self.drain()
            except Lost:
                break
            time.sleep(0.0003)
        self.flight.clear()

    def block(self, n):
        """a block as two addressed halves, each followed by the filler"""
        b = self.card.get(n)
        for h in (0, 1):
            self.stream(b"H" + struct.pack("<IB", n, h) + b[256 * h:256 * h + 256], n if h else None)
            for _ in range(FILLER):
                self.out.send_message([0xBF, 112, 0])

    def run(self):
        self.losses = 0
        while True:
            try:
                self.step()
            except Lost as e:                   # the run again from the first block lost
                j = self.job
                if e.n is not None and j and j[0] == "run" and e.n < j[1]:
                    self.job = ("run", e.n, j[2])
            now = time.monotonic()
            if now - self.t0 >= 5:
                print(f"{self.sent / (now - self.t0):.0f} blocks/s (the deck plays 43); lost and sent again: {self.losses}; "
                      f"the module: CPU {self.cpu} %, {self.passes} passes a second, {self.misses} blocks missed in its last second")
                self.sent, self.t0, self.losses = 0, now, 0

    def step(self):
        self.drain()
        j = self.job
        if j and j[0] == "boot":
            self.job = None
            self.oneoffs = []
            self.settle()
            self.boot()
        elif self.oneoffs:                      # a one-off first (the waveform's peaks), then the run carries on
            a, b = self.oneoffs.pop(0)
            for n in range(a, b):
                self.block(n)
        elif j and j[0] == "run" and j[1] < j[2]:
            n = j[1]
            self.block(n)
            if self.job is j:
                self.job = ("run", n + 1, j[2])
            elif self.job and self.job[0] == "run" and self.job[1] == n:   # extended meanwhile
                self.job = ("run", n + 1, self.job[2])
        else:
            time.sleep(0.002)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="a card image from tools/sdimage.py")
    ap.add_argument("--port", default="(emu)", help="part of the FM-1's MIDI port name")
    a = ap.parse_args()
    Host(Card(a.image), a.port).run()


if __name__ == "__main__":
    main()
