# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""jog.py: the 32 mm jog. Platter with its 6 mm shaft printed in one piece, through two
MR106ZZ (6 x 10 x 3) in the shell's boss; a printed collar glued on under the lower bearing
holds it in (hub on the upper inner race, collar on the lower one). The 4 x 2 diametric
magnet sits flush in the shaft tip, AIR_GAP over the AS5600 breakout on the base.

Print the platter upside down (shaft up). Local frame of every part: axis = z, z = 0 at its
underside; STACK gives each part's world z, on the shell's jog axis.
python3 jog.py -> build/platter.stl, collar.stl, fit_gauge.stl
"""
import math
from build123d import *
import params as M

C = (Align.CENTER, Align.CENTER, Align.MIN)
KNURL_N, KNURL_D = 36, 1.4
DIMPLE = (12.0, 0.8)          # finger dimple in the top: dia, depth
MARK = (1.0, 5.0)             # position line from the dimple out, engraved

PLATTER_Z = M.FLOOR + M.PLATTER_GAP
SHAFT_L = PLATTER_Z - M.SHAFT_BOTTOM

def platter():
    p = Cylinder(M.JOG_D / 2, M.PLATTER_H, align=C)
    try:
        p = fillet(p.edges().filter_by(lambda e: e.center().Z > M.PLATTER_H - 0.1), 0.8)
    except Exception as e:
        print("  (platter round skipped:", e, ")")
    hub = Pos(0, 0, -M.PLATTER_GAP) * Cylinder(M.BRG_INNER_OD / 2, M.PLATTER_GAP + 0.5, align=C)
    p = p.fuse(hub, shaft())
    cuts = [Pos(M.JOG_D / 2 * math.cos(a), M.JOG_D / 2 * math.sin(a), 1.0) * Cylinder(KNURL_D / 2, M.PLATTER_H, align=C)
            for a in (2 * math.pi * i / KNURL_N for i in range(KNURL_N))]
    d, dd = DIMPLE
    cuts.append(Pos(0, 0, M.PLATTER_H - dd) * Cylinder(d / 2, 2, align=C))
    cuts.append(Pos(0, d / 2 + MARK[1] / 2 + 0.5, M.PLATTER_H - 0.5) * Box(MARK[0], MARK[1], 1, align=C))
    return p.cut(*cuts)

def shaft():
    """In the platter frame: down through both bearings to SHAFT_BOTTOM, magnet pocket in the tip."""
    r, lead = M.SHAFT_D / 2, 0.4
    z0 = -SHAFT_L
    s = Pos(0, 0, z0 + lead) * Cylinder(r, SHAFT_L - lead + 0.5, align=C)
    s = s.fuse(Pos(0, 0, z0) * Cone(r - lead, r, lead, align=C))
    pocket = Pos(0, 0, z0 - 0.01) * Cylinder((M.MAGNET[0] + M.MAGNET_FIT) / 2, M.MAGNET[1] + 0.1, align=C)
    return s - pocket

def collar():
    od, h = M.COLLAR
    return Cylinder(od / 2, h, align=C) - Pos(0, 0, -1) * Cylinder(M.SHAFT_D / 2 + 0.05, h + 2, align=C)

def fit_gauge():
    """Pins at SHAFT_D -0.05 .. +0.10 to try in an MR106 (slides on by hand, no wobble) and
    holes for the collar at +0.0 .. +0.15 (snug on the shaft before glue)."""
    steps = (-0.05, 0.0, 0.05, 0.10)
    plate = Box(4 * 10, 12, 2, align=(Align.MIN, Align.CENTER, Align.MIN))
    pins = [Pos(5 + 10 * i, 2, 2) * Cylinder((M.SHAFT_D + d) / 2, 8, align=C) for i, d in enumerate(steps)]
    labels = [Pos(5 + 10 * i, -4, 1.4) * extrude(Text(f"{d * 100:+.0f}", 2.6), 1) for i, d in enumerate(steps)]
    return plate.fuse(*pins).cut(*labels)

def bearing():
    i, o, w = M.BRG
    return Cylinder(o / 2, w, align=C) - Pos(0, 0, -1) * Cylinder(i / 2, w + 2, align=C)

def magnet():
    return Cylinder(M.MAGNET[0] / 2, M.MAGNET[1], align=C)

def encoder_pcb():
    w, d, t = M.ENC_PCB
    return extrude(RectangleRounded(w, d, 2.0), t) - [Pos(x, y, -1) * Cylinder(1.75, t + 2, align=C) for x, y in M.ENC_HOLES]

def encoder_chip():
    return Box(4.9, 3.9, M.ENC_CHIP_H, align=C)

STACK = {   # world z of each part's underside, on the jog axis
    "platter":     PLATTER_Z,
    "bearing_top": M.FLOOR - M.BRG[2],
    "bearing_bot": M.BOSS_BOTTOM,
    "collar":      M.BOSS_BOTTOM - M.COLLAR[1] - 0.05,
    "magnet":      M.SHAFT_BOTTOM,
    "encoder_pcb": M.ENC_Z,
    "encoder_chip": M.ENC_Z + M.ENC_PCB[2],
}
MATES = {"seat": Pos(0, 0, 0)}

if __name__ == "__main__":
    import trimesh
    print(f"shaft {M.SHAFT_D} x {SHAFT_L:.2f} below the platter, air gap {M.AIR_GAP}, platter top {PLATTER_Z + M.PLATTER_H:.1f}")
    for name, s in (("platter", platter()), ("collar", collar()), ("fit_gauge", fit_gauge())):
        f = M.out(name + ".stl")
        export_stl(s, f, tolerance=0.02, angular_tolerance=0.1)
        m = trimesh.load(f)
        print(f, (m.bounds[1] - m.bounds[0]).round(2), "bodies:", len(m.split(only_watertight=False)), "watertight:", m.is_watertight)
