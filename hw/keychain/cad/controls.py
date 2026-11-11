# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""controls.py: CUE / PLAY caps and the fader knob, plus stand-ins for the tact switches
and the pot. Local frame: z = 0 on the deck's top surface, +z up; MATES["seat"] goes on
the shell's "cue" / "play" / "knob" mates. The pot's frame sits on the "pot" mate.

Caps go in from below: the flange under the top stops them coming out, the nub rests on the
6 x 6 tact switch. For prototyping, bought 6 x 6 switches with tall (9.5 mm) round actuators
poke through the same 8.4 mm holes without caps (see README).
python3 controls.py -> build/cue_cap.stl, play_cap.stl, knob.stl
"""
from build123d import *
import params as M

C = (Align.CENTER, Align.CENTER, Align.MIN)

def button_cap(icon):
    top_of_flange = -M.TOP_T - M.BTN_GAP
    fd, ft = M.BTN_FLANGE
    body = Pos(0, 0, top_of_flange) * Cylinder(M.BTN_D / 2, M.BTN_RISE - top_of_flange, align=C)
    try:
        body = fillet(body.edges().filter_by(lambda e: e.center().Z > M.BTN_RISE - 0.1), 0.6)
    except Exception as e:
        print("  (cap round skipped:", e, ")")
    flange = Pos(0, 0, top_of_flange - ft) * Cylinder(fd / 2, ft, align=C)
    act_top = M.MAIN_TOP + M.TACT[3] - M.H          # switch actuator, top-surface frame
    nub = Pos(0, 0, act_top) * Cylinder(M.NUB_D / 2, top_of_flange - ft - act_top, align=C)
    return body.fuse(flange, nub).cut(icon_solid(icon))

def icon_solid(icon):
    z = M.BTN_RISE - 0.4
    if icon == "play":   # play/pause
        tri = Pos(-0.9, 0, z) * extrude(Polygon((-1.4, -1.7), (1.3, 0), (-1.4, 1.7)), 1)
        bars = [Pos(x, 0, z) * Box(0.6, 3.4, 1, align=C) for x in (1.3, 2.4)]
        return tri.fuse(*bars)
    return Pos(0, 0, z) * extrude(Text("CUE", 2.3, font_style=FontStyle.BOLD), 1)

def icon_fill(icon):
    return icon_solid(icon) & Pos(0, 0, M.BTN_RISE - 0.4) * Box(20, 20, 0.35, align=C)

def tact():
    """6 x 6 SMD tact switch on the main PCB; local z = 0 on the PCB top (placed in machine.py)."""
    bx, by, bh, at = M.TACT
    return Box(bx, by, bh, align=C).fuse(Pos(0, 0, bh) * Cylinder(1.75, at - bh, align=C))

def knob():
    """Low CDJ-ish knob: tapered, push fit on the T-shaft, KNOB_LIFT above the top."""
    kx, ky = M.KNOB
    h = M.KNOB_H
    k = loft([Plane.XY * RectangleRounded(kx, ky, 1.2), Plane.XY.offset(h) * RectangleRounded(kx - 1.6, ky - 2.4, 0.8)])
    try:
        k = fillet(k.edges().filter_by(lambda e: e.center().Z > h - 0.05), 0.5)
    except Exception as e:
        print("  (knob round skipped:", e, ")")
    sa, sl = M.KNOB_SOCKET
    k = k.cut(Pos(0, 0, -0.01) * Box(sa, sl, h - 1.0 + 0.01, align=C))
    return Pos(0, 0, M.KNOB_LIFT) * k

def knob_line():
    return Pos(0, 0, M.KNOB_LIFT + M.KNOB_H - 0.3) * Box(M.KNOB[0] - 2.6, 0.6, 0.4, align=C)

def pot(lever_y=0.0):
    """Generic 45 mm pot on the "pot" mate (frame top, slot centre): frame, lever, pins."""
    fx, fy, fh = M.POT_FRAME
    frame = Pos(0, 0, -fh) * Box(fx, fy, fh, align=C)
    frame -= [Pos(0, s * M.POT_HOLES / 2, -1.5) * Cylinder(0.8, 2, align=C) for s in (-1, 1)]
    la, ll, lh = M.LEVER
    lever = Pos(0, lever_y, 0) * Box(la, ll, lh, align=C)
    pins = [Pos(px, py, -fh - M.POT_PINS_L) * Box(0.6, 0.8, M.POT_PINS_L, align=C)
            for px, py in ((-1.75, -fy / 2 + 2), (1.75, -fy / 2 + 2), (0, fy / 2 - 2))]
    return frame.fuse(lever, *pins)

MATES = {"seat": Pos(0, 0, 0)}

if __name__ == "__main__":
    import trimesh
    for name, s in (("cue_cap", button_cap("cue")), ("play_cap", button_cap("play")), ("knob", knob())):
        f = M.out(name + ".stl")
        export_stl(s, f, tolerance=0.02, angular_tolerance=0.1)
        m = trimesh.load(f)
        print(f, (m.bounds[1] - m.bounds[0]).round(2), "bodies:", len(m.split(only_watertight=False)), "watertight:", m.is_watertight)
