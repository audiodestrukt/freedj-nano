# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""machine.py: the whole puck deck. Every part is already in world coordinates (params.py
places them all from the jog axis). Checks every pair of parts for overlap, then exports GLBs
for rendering (metres, Y-up):
  build/deck.glb         assembled
  build/deck_cut.glb     section through the axis and both sensors (y >= 0, the back half)
  build/deck_inside.glb  face, screen and shell hidden: the bearing, comb, sensors and bay
python3 machine.py [--no-check]
"""
import itertools, math, sys
import numpy as np
import trimesh
from trimesh.visual.material import PBRMaterial
from PIL import Image
from build123d import *
import params as M
import shell, jog, stock

LOOK = {   # rgba, metallic, roughness
    "shell":  ((0.86, 0.87, 0.88, 1), 0, 0.55),
    "base":   ((0.12, 0.12, 0.13, 1), 0, 0.6),
    "face":   ((0.07, 0.07, 0.075, 1), 0, 0.45),
    "hub":    ((0.9, 0.45, 0.1, 1), 0, 0.5),
    "post":   ((0.3, 0.31, 0.33, 1), 0, 0.55),
    "white":  ((0.95, 0.95, 0.95, 1), 0, 0.5),
    "steel":  ((0.62, 0.63, 0.65, 1), 1, 0.3),
    "pcb":    ((0.05, 0.22, 0.12, 1), 0, 0.5),
    "pcb_blue": ((0.06, 0.12, 0.45, 1), 0, 0.5),
    "chip":   ((0.02, 0.02, 0.02, 1), 0, 0.4),
    "lens":   ((0.01, 0.01, 0.012, 1), 0, 0.06),
    "lcm":    ((0.5, 0.5, 0.52, 1), 0.7, 0.4),
    "batt":   ((0.75, 0.76, 0.78, 1), 0.6, 0.35),
    "jack":   ((0.03, 0.03, 0.03, 1), 0, 0.6),
    "sensor": ((0.08, 0.08, 0.09, 1), 0, 0.5),
}
CUT_LOOK = {"shell": ((0.55, 0.57, 0.6, 1), 0, 0.6), "face": ((0.2, 0.2, 0.22, 1), 0, 0.5),
            "steel": ((0.55, 0.65, 0.85, 1), 0.4, 0.4)}

def scene_parts():
    out = []
    add = lambda n, s, lk: out.append((n, s, lk))
    add("shell", shell.shell(), "shell")
    add("base", shell.base(), "base")
    add("post", jog.post(), "post")
    add("washer", jog.washer(), "post")
    add("bearing", stock.bearing(), "steel")
    add("hub", jog.hub(), "hub")
    add("face", jog.face(), "face")
    add("mark", jog.face(mark_only=True), "white")
    lens, lcm, spcb = stock.screen()
    add("screen_lens", lens, "lens"); add("screen_lcm", lcm, "lcm"); add("screen_pcb", spcb, "pcb_blue")
    for i, a in enumerate(M.SENS_ANG):
        add(f"sensor_{'ab'[i]}", stock.sensor(a), "sensor")
    add("main_pcb", stock.main_pcb(), "pcb")
    xp, xs = stock.xiao()
    add("xiao_pcb", xp, "pcb_blue"); add("xiao_shield", xs, "steel"); add("usb_c", stock.usb_c(), "steel")
    add("battery", stock.battery(), "batt")
    add("jack", stock.jack(), "jack")
    add("dac", stock.dac(), "chip")
    return out

# pairs meant to overlap: sensor leads solder through the board; the mark is an inlay
ALLOWED = {("sensor_a", "main_pcb"), ("sensor_b", "main_pcb"), ("face", "mark")}

def check(parts):
    bad = []
    for (na, a, _), (nb, b, _) in itertools.combinations(parts, 2):
        if (na, nb) in ALLOWED or (nb, na) in ALLOWED:
            continue
        A, B = a.bounding_box(), b.bounding_box()
        if any(a1 <= b0 or b1 <= a0 for a0, a1, b0, b1 in zip(A.min, A.max, B.min, B.max)):
            continue
        try:
            r = a & b   # newer build123d: None if they do not touch, or a list of pieces
            v = 0.0 if r is None else sum(x.volume for x in r) if isinstance(r, (list, tuple)) else r.volume
        except Exception:
            v = float("nan")
        if not v < 0.05:
            bad.append((na, nb, v))
    for na, nb, v in bad:
        print(f"  CLASH {na} x {nb}: {v:.2f} mm^3")
    print(f"fit check: {len(parts)} parts, {'no clashes' if not bad else f'{len(bad)} clashes'}")
    return not bad

def screen_mesh(n=96):
    r, z = M.ACTIVE_D / 2, M.GLASS_TOP + 0.02
    pts = [(0, 0, z)] + [(r * math.cos(2 * math.pi * i / n), r * math.sin(2 * math.pi * i / n), z) for i in range(n)]
    uv = [(0.5, 0.5)] + [(0.5 + 0.5 * math.cos(2 * math.pi * i / n), 0.5 + 0.5 * math.sin(2 * math.pi * i / n)) for i in range(n)]
    faces = [[0, 1 + i, 1 + (i + 1) % n] for i in range(n)]
    img = Image.open(M.out("screen.png")).convert("RGB")
    mat = PBRMaterial(name="screen", baseColorTexture=img, emissiveTexture=img, emissiveFactor=[1, 1, 1],
                      metallicFactor=0, roughnessFactor=0.2)
    return trimesh.Trimesh(vertices=np.array(pts), faces=faces,
                           visual=trimesh.visual.TextureVisuals(uv=np.array(uv), material=mat), process=False)

def export(parts, path, with_screen=True, look=None):
    look = {**LOOK, **(look or {})}
    scene = trimesh.Scene()
    for name, solid, lk in parts:
        f = M.out(f"viz/{name}.stl")
        export_stl(solid, f, tolerance=0.01, angular_tolerance=0.05)
        # STL has no normals and trimesh welds its vertices, so a viewer would smooth every edge
        # (holes turn into dimples). Split the mesh at creases and write the normals out.
        m = trimesh.graph.smooth_shade(trimesh.load(f), angle=np.radians(30))
        rgba, metal, rough = look[lk]
        m.visual = trimesh.visual.TextureVisuals(material=PBRMaterial(
            name=lk, baseColorFactor=rgba, metallicFactor=metal, roughnessFactor=rough,
            alphaMode="BLEND" if rgba[3] < 1 else "OPAQUE", doubleSided=True))
        scene.add_geometry(m, geom_name=name)
    if with_screen:
        scene.add_geometry(screen_mesh(), geom_name="screen")
    scene.apply_transform(trimesh.transformations.scale_matrix(0.001))
    scene.apply_transform(trimesh.transformations.rotation_matrix(-np.pi / 2, [1, 0, 0]))
    scene.export(path, include_normals=True)
    print("wrote", path)

def build(do_check=True):
    import screen_mock; screen_mock.main()
    parts = scene_parts()
    ok = check(parts) if do_check else True
    export(parts, M.out("deck.glb"))
    keep = Box(400, 400, 200, align=(Align.CENTER, Align.MIN, Align.CENTER))
    cut = []
    for name, s, lk in parts:
        bb = s.bounding_box()
        if bb.max.Y <= 0: continue
        if bb.min.Y >= 0: cut.append((name, s, lk)); continue
        c = s & keep
        if c.volume > 0.01: cut.append((name, c, lk))
    export(cut, M.out("deck_cut.glb"), with_screen=False, look=CUT_LOOK)
    hide = ("face", "mark", "shell", "screen_lens", "screen_lcm", "screen_pcb")
    export([p for p in parts if p[0] not in hide], M.out("deck_inside.glb"), with_screen=False)
    return ok

if __name__ == "__main__":
    sys.exit(0 if build("--no-check" not in sys.argv) else 1)
