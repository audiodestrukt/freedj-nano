# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""jog.py: the parts that carry and make up the jog, in world coordinates.
  post    stands on the deck plate, carries the 61808's inner race and the screen on top
  washer  clamps the inner race down onto the post's shoulder (under the screen's feet)
  hub     the outer race presses into it; its toothed comb runs through the opto-interrupters
  face    the jog you touch: the ring round the glass and the knurled side, glued onto the hub
python3 jog.py -> build/{post,washer,hub,face}.stl (in print orientation)
"""
import math
from build123d import *
import params as M

C = (Align.CENTER, Align.CENTER, Align.MIN)

def tube(od, idia, z0, z1):
    t = Pos(0, 0, z0) * Cylinder(od / 2, z1 - z0, align=C)
    return t - Pos(0, 0, z0 - 1) * Cylinder(idia / 2, z1 - z0 + 2, align=C) if idia > 0 else t

def post():
    step_z = M.BRG_Z - M.STEP[1]
    p = tube(M.POST_LOW_D, M.POST_BORE, M.PLATE_TOP, step_z)
    p = p.fuse(tube(M.STEP[0], M.POST_BORE, step_z, M.BRG_Z))
    p = p.fuse(tube(M.BRG[0], M.POST_BORE, M.BRG_Z, M.POST_TOP))
    # lead-in chamfer for the bearing
    # 0.5 x 45 deg off the top outer edge: cut away everything outside a cone that runs from
    # the post's radius 0.5 below the top in to 0.5 smaller at the top
    r = M.BRG[0] / 2
    p -= Pos(0, 0, M.POST_TOP - 0.6) * Cylinder(r + 2, 1.2, align=C) \
        - Pos(0, 0, M.POST_TOP - 0.6) * Cone(r + 0.1, r - 0.6, 0.7, align=C) \
        - Pos(0, 0, M.POST_TOP - 0.6) * Cylinder(r - 0.6, 1.2, align=C)
    cuts = []
    for a in M.SENS_ANG:   # flats for the sensors' inner arms
        cuts.append(Rot(0, 0, a) * Pos(M.POST_FLAT, 0, M.PLATE_TOP - 1) *
                    Box(10, M.SENS_W + 4, M.SENS_TOP + 0.4 - M.PLATE_TOP + 1, align=(Align.MIN, Align.CENTER, Align.MIN)))
    top_solid = M.POST_TOP - M.POST_CB[1]
    for a in M.SCR_HOLE_ANG:   # screen screws come up from below
        x, y = M.ang(a, M.SCR_HOLE_R)
        cuts.append(Pos(x, y, M.PLATE_TOP - 1) * Cylinder(1.2, M.POST_TOP - M.PLATE_TOP + 2, align=C))
        cuts.append(Pos(x, y, M.PLATE_TOP - 1) * Cylinder(M.POST_CB[0] / 2, top_solid - M.PLATE_TOP + 1, align=C))
    for a in M.POST_FIX_ANG:   # M2 thread-formers up through the plate
        x, y = M.ang(a, M.POST_FIX_R)
        cuts.append(Pos(x, y, M.PLATE_TOP - 1) * Cylinder(0.8, 6.0, align=C))
    return p.cut(*cuts)

def washer():
    od, idia, t = M.WASHER
    w = tube(od, idia, M.BRG_TOP, M.BRG_TOP + t)
    return w.cut(*[Pos(*M.ang(a, M.SCR_HOLE_R), M.BRG_TOP - 1) * Cylinder(1.2, t + 2, align=C) for a in M.SCR_HOLE_ANG])

def hub():
    r0, r1 = M.COMB_R - M.COMB_T / 2, M.COMB_R + M.COMB_T / 2
    comb = tube(2 * r1, 2 * r0, M.COMB_BOT, M.FLANGE_Z + 0.01)
    pitch = 2 * math.pi * M.COMB_R / M.TEETH
    slot = Pos(M.COMB_R, 0, M.COMB_BOT - 1) * Box(M.COMB_T + 2, pitch / 2, M.SLOT_TOP - M.COMB_BOT + 1,
                                                    align=(Align.CENTER, Align.CENTER, Align.MIN))
    comb = comb.cut(*[Rot(0, 0, i * 360 / M.TEETH) * slot for i in range(M.TEETH)])
    flange = tube(M.HUB_OUT, M.FLANGE_IN, M.FLANGE_Z, M.BRG_Z)
    wall = tube(M.HUB_OUT, M.HUB_IN, M.BRG_Z, M.BRG_TOP)
    return comb.fuse(flange, wall)

def face(mark_only=False):
    top0 = M.FACE_TOP - M.FACE_T
    mw, ml, md = M.MARK
    mr = (M.FACE_ID / 2 + M.R_OUT) / 2
    mark = Pos(0, mr, M.FACE_TOP - md) * Box(mw, ml, md, align=C)
    if mark_only:
        return mark
    skin = tube(2 * M.R_OUT, M.FACE_ID, top0, M.FACE_TOP)
    rim = tube(2 * M.R_OUT, 2 * (M.R_OUT - M.RIM_T), M.PLATE_TOP + M.RIM_GAP, top0 + 0.01)
    f = skin.fuse(rim)
    try:   # round the outer top edge, chamfer the edge round the glass
        f = fillet(f.edges().filter_by(GeomType.CIRCLE).group_by(Axis.Z)[-1].sort_by(SortBy.RADIUS)[-1:], M.EDGE_R)
        f = chamfer(f.edges().filter_by(GeomType.CIRCLE).group_by(Axis.Z)[-1].sort_by(SortBy.RADIUS)[:1], 0.6)
    except Exception as e:
        print("  (face edge round skipped:", e, ")")
    inner = tube(M.FACE_ID + 2 * M.FACE_INNER_WALL, M.FACE_ID, M.FACE_INNER_LOW, top0 + 0.01)
    press = tube(M.HUB_IN - 0.2, M.PRESS_IN, M.BRG_TOP, top0 + 0.01)
    sid, sw, sdown = M.SOCKET
    sock = tube(sid + 2 * sw, sid, M.BRG_TOP - sdown, top0 + 0.01)
    f = f.fuse(inner, press, sock)
    n, dpt = M.KNURL
    flute = Pos(M.R_OUT + 0.6 - dpt, 0, M.PLATE_TOP) * Cylinder(0.6, top0 - M.PLATE_TOP - 0.6, align=C)
    f = f.cut(*[Rot(0, 0, i * 360 / n) * flute for i in range(n)])
    return f - mark

def flip(s, z):
    """Print orientation: upside down, top face (at z) on the bed."""
    return Pos(0, 0, z) * Rot(180, 0, 0) * s

PRINT = {   # name: (part, to print orientation)
    "post": (post, lambda s: Pos(0, 0, -M.PLATE_TOP) * s),
    "washer": (washer, lambda s: Pos(0, 0, -M.BRG_TOP) * s),
    "hub": (hub, lambda s: flip(s, M.BRG_TOP)),
    "face": (face, lambda s: flip(s, M.FACE_TOP)),
}

if __name__ == "__main__":
    import trimesh
    for name, (fn, orient) in PRINT.items():
        f = M.out(f"{name}.stl")
        export_stl(orient(fn()), f, tolerance=0.03, angular_tolerance=0.1)
        m = trimesh.load(f)
        print(f, (m.bounds[1] - m.bounds[0]).round(1), "bodies:", len(m.split(only_watertight=False)), "watertight:", m.is_watertight)
