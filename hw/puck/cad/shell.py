# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""shell.py: the static body, in world coordinates.
  shell  the deck plate (post on top, sensor windows) and the wall down to the floor, with
         the bosses the base screws into, the USB-C and jack openings
  base   flat plate inside the wall; four M2 countersunk screws go up through it and the
         main PCB (clamped on its standoff rings) into the shell's bosses
python3 shell.py -> build/{shell,base}.stl (in print orientation)
"""
from build123d import *
import params as M
from jog import tube

C = (Align.CENTER, Align.CENTER, Align.MIN)

def shell():
    s = tube(2 * M.R_OUT, 2 * M.R_IN, 0, M.PLATE_TOP)
    s = s.fuse(Pos(0, 0, M.PLATE_Z) * Cylinder(M.R_IN + 0.01, M.PLATE_T, align=C))
    try:   # round the bottom outer edge
        s = fillet(s.edges().filter_by(GeomType.CIRCLE).group_by(Axis.Z)[0].sort_by(SortBy.RADIUS)[-1:], M.EDGE_R)
    except Exception as e:
        print("  (shell edge round skipped:", e, ")")
    bosses = [Pos(*M.ang(a, M.BOSS_R), M.MAIN_TOP) * Cylinder(M.BOSS_D / 2, M.PLATE_Z - M.MAIN_TOP + 0.01, align=C)
              for a in M.BOSS_ANG]
    s = s.fuse(*bosses)
    cuts = [Pos(0, 0, M.PLATE_Z - 1) * Cylinder(M.POST_BORE / 2, M.PLATE_T + 2, align=C)]   # cable
    for a in M.BOSS_ANG:
        cuts.append(Pos(*M.ang(a, M.BOSS_R), M.MAIN_TOP - 0.01) * Cylinder(M.BOSS_PILOT[0] / 2, M.BOSS_PILOT[1], align=C))
    for a in M.SENS_ANG:   # windows the sensors come up through
        cuts.append(Rot(0, 0, a) * Pos(M.SENS_R, 0, M.PLATE_Z - 1) * Box(M.SENS_L + 0.8, M.SENS_W + 0.8, M.PLATE_T + 2, align=C))
    for a in M.SCR_HOLE_ANG:   # screwdriver to the screen screws
        cuts.append(Pos(*M.ang(a, M.SCR_HOLE_R), M.PLATE_Z - 1) * Cylinder(M.POST_CB[0] / 2 + 0.2, M.PLATE_T + 2, align=C))
    for a in M.POST_FIX_ANG:   # countersunk M2 up into the post
        x, y = M.ang(a, M.POST_FIX_R)
        cuts.append(Pos(x, y, M.PLATE_Z - 1) * Cylinder(1.15, M.PLATE_T + 2, align=C))
        cuts.append(Pos(x, y, M.PLATE_Z - 0.01) * Cone(2.05, 0.9, 1.15, align=C))
    # jack: window over its body (it's taller than the bay), hole for the barrel
    jl, jw, jh = M.JACK
    cuts.append(Pos(M.JACK_X0 + jl / 2, M.JACK_Y, M.PLATE_Z - 1) * Box(jl + 0.8, jw + 0.8, M.PLATE_T + 2, align=C))
    cuts.append(Pos(M.JACK_X0 - 6, M.JACK_Y, M.MAIN_TOP + jh / 2) * Rot(0, 90, 0) * Cylinder(3.0, 8, align=C))
    # USB-C: opening plus a relief outside for a stick's overmould
    uw, uh, _ = M.USBC
    uz = M.MAIN_TOP + M.XIAO[2] + uh / 2
    cuts.append(Pos(M.XIAO_C[0], M.R_OUT, uz) * Rot(90, 0, 0) * extrude(RectangleRounded(uw + 0.8, uh + 0.8, 1.6), 6, both=True))
    cuts.append(Pos(M.XIAO_C[0], M.R_OUT + 1, uz) * Rot(90, 0, 0) * extrude(RectangleRounded(12.5, 7.0, 2.5), 1.6))
    return s.cut(*cuts)

def base():
    b = Pos(0, 0, 0) * Cylinder(M.R_IN - 0.25, M.BASE_T, align=C)
    rings = [Pos(*M.ang(a, M.BOSS_R), M.BASE_T - 0.01) * Cylinder(M.BOSS_D / 2, M.MAIN_Z - M.BASE_T + 0.01, align=C)
             for a in M.BOSS_ANG]
    b = b.fuse(*[r & Cylinder(M.R_IN - 0.25, 10, align=C) for r in rings])
    cuts = []
    for a in M.BOSS_ANG:
        x, y = M.ang(a, M.BOSS_R)
        cuts.append(Pos(x, y, -1) * Cylinder(1.15, M.MAIN_Z + 2, align=C))
        cuts.append(Pos(x, y, -0.01) * Cone(2.05, 0.9, 1.15, align=C))
    return b.cut(*cuts)

PRINT = {
    "shell": (shell, lambda s: Pos(0, 0, M.PLATE_TOP) * Rot(180, 0, 0) * s),   # upside down, deck plate on the bed
    "base": (base, lambda s: s),
}

if __name__ == "__main__":
    import trimesh
    for name, (fn, orient) in PRINT.items():
        f = M.out(f"{name}.stl")
        export_stl(orient(fn()), f, tolerance=0.03, angular_tolerance=0.1)
        m = trimesh.load(f)
        print(f, (m.bounds[1] - m.bounds[0]).round(1), "bodies:", len(m.split(only_watertight=False)), "watertight:", m.is_watertight)
