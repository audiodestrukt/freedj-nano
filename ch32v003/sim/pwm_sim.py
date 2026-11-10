#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""What the CH32V003's PWM audio output sounds like, before there's hardware.

Models TIM1 the way phase 1 sets it up: two 8-bit PWM channels (high byte, low byte),
either edge-aligned at 48 MHz / 272 = 176.4 kHz (duty updated every 8 periods: 22.05 kHz)
or centre-aligned at 48 MHz / 544 = 88.2 kHz (every 4), optionally with N-times linear
interpolation on the chip. The pins drive the resistor sum
(R1 : R2, 1 : 256, plus the GPIO's output resistance and resistor tolerances) into the
analog filter. The network is solved exactly per PWM period (matrix exponentials of
the circuit's state-space model, piecewise-constant pin voltages), averaged over each
period, which removes the carrier and keeps everything audible, then decimated to
44.1 kHz.

  pwm_sim.py --selftest                 check the period algebra against tick-by-tick
                                        stepping, and the filter against ngspice
  pwm_sim.py                            measure every variant (THD+N, images, ripple)
  pwm_sim.py --in song.wav --out dir    also render each variant as a WAV for listening

Not modelled: supply noise beyond --vdd-noise, ground bounce, pin edge rates, the op-amp.
"""
import argparse, math, os, re, shutil, subprocess, sys, tempfile, wave

import numpy as np
import scipy.linalg as sla
import scipy.signal as sig

FS = 22050
COUNTS = 272                     # ARR + 1; repetition counter 7 either way
DT = 1.0 / (FS * 8 * COUNTS)     # one timer tick (the chip: 48 MHz, 22,059 Hz, 0.04 % fast)
FOUT = 44100

# edge: counts up, pin high for `duty` ticks from the period start: 176.4 kHz, 8 per sample.
# centre: counts up and down, pin high for 2*duty ticks centred in the period: 88.2 kHz,
# 4 per sample. The pulse's centre doesn't move with the duty.
ALIGN = {"edge": dict(ticks=COUNTS, periods=8), "centre": dict(ticks=2 * COUNTS, periods=4)}

HERE = os.path.dirname(os.path.abspath(__file__))


# ---------------------------------------------------------------- circuit

class Circuit:
    """Pins -> resistor sum -> filter, as x' = A x + B [v_hi, v_lo], out = C x."""

    def __init__(self, filt="rc2", r1=1e3, r2=255e3, rout=40.0, e1=0.0, e2=0.0,
                 lo=True, vdd=3.3, **vals):
        self.filt, self.vdd, self.lo = filt, vdd, lo
        self.r1, self.r2, self.rout, self.e1, self.e2 = r1, r2, rout, e1, e2
        G1 = 1.0 / (r1 * (1 + e1) + rout)
        G2 = 1.0 / (r2 * (1 + e2) + rout) if lo else 0.0
        self.v = dict(DEFAULTS[filt]); self.v.update(vals)
        v = self.v
        if filt == "rc2":
            # sum node N (C1) -> R3 -> M (C2) -> buffer
            C1, R3, C2 = v["c1"], v["r3"], v["c2"]; G3 = 1 / R3
            A = [[-(G1 + G2 + G3) / C1, G3 / C1],
                 [G3 / C2, -G3 / C2]]
            B = [[G1 / C1, G2 / C1], [0, 0]]
            C = [0, 1]
        elif filt == "sk3":
            # sum node N (C1) -> unity-gain Sallen-Key (Ra, Rb, Ca to output, Cb to ground)
            # states: v1, vca = va - vout, vout
            C1, Ra, Rb, Ca, Cb = v["c1"], v["ra"], v["rb"], v["ca"], v["cb"]
            Ga, Gb = 1 / Ra, 1 / Rb
            A = [[-(G1 + G2 + Ga) / C1, Ga / C1, Ga / C1],
                 [Ga / Ca, -(Ga + Gb) / Ca, -Ga / Ca],
                 [0, Gb / Cb, 0]]
            B = [[G1 / C1, G2 / C1], [0, 0], [0, 0]]
            C = [0, 0, 1]
        else:
            raise ValueError(filt)
        self.A, self.B, self.C = np.array(A, float), np.array(B, float), np.array(C, float)
        self.n = len(C)

    def response(self, f):
        """Complex gain from the high-byte pin to the output, normalised to 1 at DC."""
        s = 2j * np.pi * np.atleast_1d(f)
        I = np.eye(self.n)
        h = np.array([self.C @ np.linalg.solve(si * I - self.A, self.B[:, 0]) for si in s])
        h0 = self.C @ np.linalg.solve(-self.A, self.B[:, 0])
        return h / h0

    def label(self):
        v = ", ".join(f"{k.upper()} {eng(x)}" for k, x in self.v.items())
        r2 = f"R2 {eng(self.r2)} {pct(self.e2)}" if self.lo else "no R2"
        return f"R1 {eng(self.r1)} {pct(self.e1)}, {r2}, Rout {self.rout:.0f}, {v}"


DEFAULTS = {
    # 2nd-order passive, both poles ~10.6 kHz (the plan in DECK10C.md)
    "rc2": dict(c1=15e-9, r3=10e3, c2=1.5e-9),
    # a pole at ~10 kHz + a Q=1 Sallen-Key at ~10 kHz on the MCP6002 that's there anyway:
    # ~3rd-order Butterworth
    "sk3": dict(c1=15e-9, ra=10e3, rb=10e3, ca=3.3e-9, cb=820e-12),
}


def eng(x):
    for m, p in ((1e-12, "p"), (1e-9, "n"), (1e-6, "u"), (1, ""), (1e3, "k"), (1e6, "M")):
        if abs(x) < m * 1000:
            return f"{x / m:.3g}{p}"
    return f"{x:g}"


def pct(e):
    return f"{e * 100:+.1f}%" if e else ""


# ---------------------------------------------------------------- exact per-period model

class Model:
    """Tables for one PWM period: a pin high for one pulse per period, low otherwise."""

    def __init__(self, c: Circuit, align="edge"):
        self.c, self.align = c, align
        P = self.ticks = ALIGN[align]["ticks"]
        self.periods = ALIGN[align]["periods"]
        self.fpwm = FS * self.periods
        n, A = c.n, c.A
        self.Phi, self.Psi = [], []                 # e^{A k dt}, int_0^{k dt} e^{A t} dt
        aug = np.zeros((2 * n, 2 * n)); aug[:n, :n] = A; aug[:n, n:] = np.eye(n)
        for k in range(P + 1):
            E = sla.expm(aug * k * DT)
            self.Phi.append(E[:n, :n]); self.Psi.append(E[:n, n:])
        Ainv = np.linalg.inv(A)
        self.G = np.zeros((2, 256, n))             # state at period end from the pulse
        self.H = np.zeros((2, 256, n))             # integral of the state over the period
        for ch in range(2):
            b = c.B[:, ch] * c.vdd
            for d in range(256):
                s, e = self.pulse(d)
                pw = self.Psi[e - s] @ b
                self.G[ch, d] = self.Phi[P - e] @ pw
                self.H[ch, d] = Ainv @ (pw - (e - s) * DT * b) + self.Psi[P - e] @ pw
        self.PhiT, self.PsiT, self.T = self.Phi[P], self.Psi[P], P * DT

    def pulse(self, d):
        """First and last+1 tick the pin is high, for duty d."""
        return (0, d) if self.align == "edge" else (COUNTS - d, COUNTS + d)

    def run(self, dh, dl, vdd_scale=None):
        """Duty per PWM period (0..255 each) -> period-averaged output voltage."""
        g = self.G[0][dh] + self.G[1][dl]
        h = self.H[0][dh] + self.H[1][dl]
        if vdd_scale is not None:
            g = g * vdd_scale[:, None]; h = h * vdd_scale[:, None]
        lam, V = np.linalg.eig(self.PhiT)
        Vi = np.linalg.inv(V)
        q = g @ Vi.T                                # in the eigenbasis
        z0 = np.linalg.solve(np.eye(self.c.n) - self.PhiT, g[0]) @ Vi.T   # start settled
        z = np.empty_like(q, dtype=complex)
        for i, l in enumerate(lam):
            z[:, i] = sig.lfilter([0, 1], [1, -l], q[:, i], zi=[z0[i]])[0]
        x = np.real(z @ V.T)                        # state at each period start
        return (x @ self.PsiT.T + h) @ self.c.C / self.T

    def ripple(self, dh=128, dl=0):
        """Peak-to-peak carrier ripple at the output, steady at one duty, volts."""
        n, A, c = self.c.n, self.c.A, self.c
        x = np.linalg.solve(np.eye(n) - self.PhiT, self.G[0][dh] + self.G[1][dl])
        P1, Q1 = self.Phi[1], self.Psi[1]
        (sh, eh), (sl, el) = self.pulse(dh), self.pulse(dl)
        ys = []
        for t in range(self.ticks):
            u = np.array([c.vdd * (sh <= t < eh), c.vdd * (sl <= t < el)])
            x = P1 @ x + Q1 @ (c.B @ u)
            ys.append(c.C @ x)
        return max(ys) - min(ys)


# ---------------------------------------------------------------- the chip's side

def duties(x16, periods, up=1, lo=True):
    """22.05 kHz int16 -> per-PWM-period duties, as the firmware would make them:
    optional N-times linear interpolation in integer shifts, then lite_pwm()'s byte split."""
    x = np.asarray(x16, np.int32)
    if up > 1:
        sh = int(math.log2(up)); assert 1 << sh == up and up <= periods
        nxt = np.append(x[1:], x[-1])
        k = np.arange(up)[None, :]
        x = (x[:, None] + (((nxt - x)[:, None] * k) >> sh)).ravel()
    u = (x + 32768).astype(np.int32)
    u = np.repeat(u, periods // up)
    dh = u >> 8
    dl = (u & 255) if lo else np.zeros_like(u)
    return dh, dl


def render(m: Model, x16, up=1, vdd_noise=0.0, seed=1):
    """int16 at 22.05 kHz -> float at 44.1 kHz in int16 units (gain and offset removed)."""
    c = m.c
    dh, dl = duties(x16, m.periods, up, c.lo)
    vs = None
    if vdd_noise:
        rng = np.random.default_rng(seed)
        vs = 1 + rng.standard_normal(len(dh)) * vdd_noise / c.vdd
    y = m.run(dh, dl, vs)
    lo_, hi_ = (m.run(np.array([a] * 4), np.array([b] * 4))[-1] for a, b in ((0, 0), (255, 255 if c.lo else 0)))
    top = 65535 if c.lo else 65280
    y = (y - lo_) / (hi_ - lo_) * top - 32768
    return sig.resample_poly(y, 1, m.fpwm // FOUT)


def reference(x16):
    """A perfect 22.05 kHz DAC: the same samples band-limited to 11 kHz, at 44.1 kHz."""
    return sig.resample_poly(np.asarray(x16, float), 2, 1)


# ---------------------------------------------------------------- measurements

def tone(f, dbfs, secs=1.5):
    t = np.arange(int(FS * secs)) / FS
    return np.round(32767 * 10 ** (dbfs / 20) * np.sin(2 * np.pi * f * t)).astype(np.int16)


def spectrum(y, fs=FOUT):
    y = y[int(0.3 * fs):]
    y = y - y.mean()
    w = sig.windows.blackmanharris(len(y))
    Y = np.abs(np.fft.rfft(y * w)) ** 2
    f = np.fft.rfftfreq(len(y), 1 / fs)
    return f, Y


def band(f, Y, lo, hi):
    return Y[(f >= lo) & (f <= hi)].sum()


def thdn(y, f0):
    """THD+N in 20 Hz-20 kHz, dB relative to the tone."""
    f, Y = spectrum(y)
    near = np.abs(f - f0) < 25
    pf = Y[near].sum()
    rest = Y[(f >= 20) & (f <= 20000) & ~near].sum()
    return 10 * np.log10(rest / pf)


def image(y, f0):
    """Level of the 22.05 kHz image (22050 - f0) against the tone, dB."""
    f, Y = spectrum(y)
    pf = Y[np.abs(f - f0) < 25].sum()
    pi = Y[np.abs(f - (FS - f0)) < 25].sum()
    return 10 * np.log10(pi / pf)


def idle(y):
    """Noise on silence, 20 Hz-20 kHz, dB against a full-scale sine."""
    f, Y = spectrum(y)
    f1, Y1 = spectrum(32767 * np.sin(2 * np.pi * 1000 * np.arange(len(y)) / FOUT))
    return 10 * np.log10(band(f, Y, 20, 20000) / Y1.sum() + 1e-30)


def measure(m, up=1, vdd_noise=0.0):
    """None for m: mu-law tones through a perfect 22.05 kHz DAC."""
    if m is None:
        run = lambda x: reference(to_track(x))
    else:
        run = lambda x: render(m, x, up, vdd_noise)
    r = {}
    r["thdn_6"] = thdn(run(tone(1000, -6)), 1000)
    r["thdn_6_5k"] = thdn(run(tone(5000, -6)), 5000)
    r["thdn_40"] = thdn(run(tone(1000, -40)), 1000)
    r["img_10k"] = image(run(tone(10000, -6)), 10000)
    r["idle"] = idle(run(np.zeros(int(FS * 1.5), np.int16)))
    if m is not None:
        h = np.abs(m.c.response([10000, 20000]))
        r["h10k"], r["h20k"] = 20 * np.log10(h)
        r["ripple_mv"] = m.ripple() * 1000
    return r


# ---------------------------------------------------------------- variants

def variants(rout=40.0):
    """(name, circuit, alignment, interpolation)."""
    w = dict(e1=+0.01, e2=-0.01, rout=rout)     # 1 % parts, worst way round
    k = dict(r2=267e3, **w)                     # 267k = 256 x (1k + Rout), nearest E96
    return [
        ("8-bit, one pin, edge, rc2",       Circuit("rc2", lo=False, rout=rout), "edge", 1),
        ("dual, exact 256:1, edge, rc2",    Circuit("rc2", r2=256e3, rout=0), "edge", 1),
        ("dual, 255k 1%, edge, rc2",        Circuit("rc2", **w), "edge", 1),
        ("dual, 267k 1%, edge, rc2",        Circuit("rc2", **k), "edge", 1),
        ("dual, 267k 1%, centre, rc2",      Circuit("rc2", **k), "centre", 1),
        ("dual, 267k 1%, centre, sk3",      Circuit("sk3", **k), "centre", 1),
        ("dual, 267k 1%, centre, sk3, 2x",  Circuit("sk3", **k), "centre", 2),
        ("dual, 267k 1%, centre, sk3, 4x",  Circuit("sk3", **k), "centre", 4),
    ]


def slug(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


# ---------------------------------------------------------------- I/O

def read_wav(path):
    with wave.open(path) as w:
        ch, rate, sw, n = w.getnchannels(), w.getframerate(), w.getsampwidth(), w.getnframes()
        assert sw == 2, "16-bit WAVs only"
        x = np.frombuffer(w.readframes(n), np.int16).reshape(-1, ch).astype(float).mean(axis=1)
    if rate != FS:
        g = math.gcd(rate, FS)
        x = sig.resample_poly(x, FS // g, rate // g)
    return x


def ulaw_table():
    src = open(os.path.join(HERE, "..", "lite", "ulaw.c")).read()
    body = src[src.index("{") + 1: src.index("}")]
    return np.array([int(v) for v in re.findall(r"-?\d+", body)], np.int32)


def to_track(x):
    """Float mono at 22.05 kHz -> what the deck stores and decodes: mu-law, lite_ulaw[]."""
    tab = ulaw_table()
    order = np.argsort(tab); st = tab[order]
    x = np.clip(np.round(x), -32768, 32767)
    i = np.clip(np.searchsorted(st, x), 1, len(st) - 1)
    pick = np.where(np.abs(st[i - 1] - x) <= np.abs(st[i] - x), i - 1, i)
    return st[pick].astype(np.int16)


def write_wav(path, y, rate=FOUT):
    y = np.clip(np.round(y), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(y.tobytes())


# ---------------------------------------------------------------- self-test

def selftest():
    ok = True
    # 1. the per-period tables against stepping one timer tick at a time
    rng = np.random.default_rng(3)
    for c, al in ((Circuit("rc2", e1=0.01, e2=-0.01), "edge"), (Circuit("sk3", r2=267e3), "centre")):
        m = Model(c, al)
        N = 40
        dh = rng.integers(0, 256, N); dl = rng.integers(0, 256, N)
        fast = m.run(dh, dl)
        x = np.linalg.solve(np.eye(c.n) - m.PhiT, m.G[0][dh[0]] + m.G[1][dl[0]])
        P1, Q1, Ai = m.Phi[1], m.Psi[1], np.linalg.inv(c.A)
        slow = []
        for k in range(N):
            acc = 0.0
            (sh, eh), (sl, el) = m.pulse(dh[k]), m.pulse(dl[k])
            for t in range(m.ticks):
                bu = c.B @ np.array([c.vdd * (sh <= t < eh), c.vdd * (sl <= t < el)])
                acc += c.C @ (Q1 @ x + Ai @ (Q1 @ bu - DT * bu))
                x = P1 @ x + Q1 @ bu
            slow.append(acc / m.T)
        err = np.max(np.abs(fast - np.array(slow)))
        good = err < 1e-9
        ok &= good
        print(f"  {c.filt}, {al}: period tables vs tick stepping, max error {err:.1e} V  {'ok' if good else 'FAIL'}")
    # 2. the filter against ngspice
    if shutil.which("ngspice"):
        for c in (Circuit("rc2"), Circuit("sk3", r2=267e3)):
            f, h = spice_ac(c)
            ours = 20 * np.log10(np.abs(c.response(f)))
            err = np.max(np.abs(ours - h))
            good = err < 0.05
            ok &= good
            print(f"  {c.filt}: response vs ngspice, max difference {err:.3f} dB  {'ok' if good else 'FAIL'}")
    else:
        print("  ngspice not found: filter cross-check skipped")
    print("selftest", "passed" if ok else "FAILED")
    return ok


def spice_ac(c):
    """AC response from the high-byte pin, normalised at 10 Hz, from an ngspice netlist."""
    v = c.v
    r1 = c.r1 * (1 + c.e1) + c.rout
    r2 = c.r2 * (1 + c.e2) + c.rout
    net = [f"* {c.filt}", "Vh ph 0 dc 0 ac 1", "Vl pl 0 dc 0", f"R1 ph n {r1}", f"R2 pl n {r2}"]
    if c.filt == "rc2":
        net += [f"C1 n 0 {v['c1']}", f"R3 n m {v['r3']}", f"C2 m 0 {v['c2']}", "Eo out 0 m 0 1"]
    else:
        net += [f"C1 n 0 {v['c1']}", f"Ra n a {v['ra']}", f"Rb a b {v['rb']}",
                f"Ca a out {v['ca']}", f"Cb b 0 {v['cb']}", "Eo out 0 b 0 1"]
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "ac.txt")
        net += [".control", "set wr_singlescale", "ac dec 20 10 200k",
                f"wrdata {out} vdb(out)", "quit", ".endc", ".end"]
        cir = os.path.join(d, "f.cir")
        open(cir, "w").write("\n".join(net) + "\n")
        subprocess.run(["ngspice", "-b", cir], capture_output=True, check=True)
        a = np.loadtxt(out)
    return a[:, 0], a[:, 1] - a[0, 1]


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--in", dest="inp", help="a 16-bit WAV to render through every variant")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "out"))
    ap.add_argument("--secs", type=float, default=30, help="render at most this much of --in")
    ap.add_argument("--raw", action="store_true", help="input is already the deck's output (skip mu-law)")
    ap.add_argument("--rout", type=float, default=40.0, help="GPIO output resistance, ohms (a guess)")
    ap.add_argument("--vdd-noise", type=float, default=0.0, help="supply noise, volts rms, white")
    a = ap.parse_args()

    if a.selftest:
        sys.exit(0 if selftest() else 1)

    vs = variants(a.rout)
    print(f"Vdd 3.3 V, Rout {a.rout:.0f} ohm (guess), supply noise {a.vdd_noise * 1e3:.1f} mV rms. "
          f"THD+N in 20 Hz-20 kHz, tones at -6 dBFS unless marked.\n")
    hdr = (f"{'variant':34s} {'1k':>6s} {'5k':>6s} {'1k-40':>6s} {'idle':>6s} {'img10k':>7s} "
           f"{'@10k':>5s} {'@20k':>6s} {'ripple':>9s}")
    print(hdr); print("-" * len(hdr))
    db = lambda v: f"{v:6.1f}" if v > -200 else "   -  "
    rows = [("mu-law track, perfect DAC", None, None, 1)] + vs
    for name, c, al, up in rows:
        r = measure(Model(c, al) if c else None, up, a.vdd_noise)
        tail = (f" {r['h10k']:5.1f} {r['h20k']:6.1f} {r['ripple_mv']:6.1f}mVpp" if c else "")
        print(f"{name:34s} {db(r['thdn_6'])} {db(r['thdn_6_5k'])} {db(r['thdn_40'])} {db(r['idle'])} "
              f"{r['img_10k']:7.1f}{tail}")
    print("\nidle: noise on silence against full scale.  img10k: the 12.05 kHz image of a 10 kHz tone.")
    print("@10k/@20k: the filter alone, dB.  ripple: carrier left at the output at mid-scale.")
    for name, c, al, up in vs:
        print(f"  {name}: {c.label()}")

    if a.inp:
        os.makedirs(a.out, exist_ok=True)
        x = read_wav(a.inp)[: int(a.secs * FS)]
        x16 = np.clip(np.round(x), -32768, 32767).astype(np.int16) if a.raw else to_track(x)
        a.out = os.path.normpath(a.out)
        write_wav(os.path.join(a.out, "pwm_source.wav"), reference(x))
        write_wav(os.path.join(a.out, "pwm_reference.wav"), reference(x16))
        print(f"\n{a.out}/pwm_source.wav     the input, mono 22.05 kHz, before mu-law")
        print(f"{a.out}/pwm_reference.wav  the track as stored, through a perfect DAC: A/B against this")
        for name, c, al, up in vs:
            p = os.path.join(a.out, f"pwm_{slug(name)}.wav")
            write_wav(p, render(Model(c, al), x16, up, a.vdd_noise))
            print(f"{p}")


if __name__ == "__main__":
    main()
