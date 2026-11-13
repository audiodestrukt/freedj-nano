# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""base.py: base plate. Drops inside the shell walls, four M2 x 6 countersunk screws into the
corner bosses. Carries the AS5600 breakout on four short bosses (M2 thread-formers from
above) and a shallow pocket that locates the LiPo.
python3 base.py -> build/<screen>/base.stl
"""
from build123d import *
import params as M
from shell import posts

C = (Align.CENTER, Align.CENTER, Align.MIN)
CLEAR = 0.25
ENC_BOSS_D = 4.5
BATT_POCKET = 0.6     # into the plate, 0.5 mm all round

def part():
    w, d = M.W - 2 * (M.WALL + CLEAR), M.D - 2 * (M.WALL + CLEAR)
    p = Pos(M.W / 2, M.D / 2, 0) * extrude(RectangleRounded(w, d, M.INNER_R), M.BASE_T)
    jx, jy = M.JOG_C
    bosses, cuts = [], []
    for x, y in M.ENC_HOLES:
        bosses.append(Pos(jx + x, jy + y, M.BASE_T - 0.5) * Cylinder(ENC_BOSS_D / 2, M.ENC_STANDOFF + 0.5, align=C))
        cuts.append(Pos(jx + x, jy + y, 0.5) * Cylinder(0.8, M.ENC_Z, align=C))   # M2 pilot, blind
    for x, y in posts():
        cuts.append(Pos(x, y, -1) * Cylinder(1.15, M.BASE_T + 2, align=C))
        cuts.append(Pos(x, y, -0.01) * Cone(2.05, 1.15, 0.91, align=C))
    bx, by, _ = M.BATT
    cuts.append(Pos(*M.BATT_C, M.BASE_T - BATT_POCKET) * Box(bx + 1, by + 1, 1, align=C))
    return p.fuse(*bosses).cut(*cuts)

MATES = {"to_shell": Pos(0, 0, 0)}

if __name__ == "__main__":
    f = M.out("base.stl")
    export_stl(part(), f, tolerance=0.03, angular_tolerance=0.1)
    import trimesh; m = trimesh.load(f)
    print(f, (m.bounds[1] - m.bounds[0]).round(1), "bodies:", len(m.split(only_watertight=False)), "watertight:", m.is_watertight)
