# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""machine.py: the whole keychain deck, every part placed by its mates. Checks every pair of
parts for overlap, then exports GLBs for rendering (metres, Y-up):
  build/deck.glb         assembled
  build/deck_cut.glb     section through the jog axis (x <= axis)
  build/deck_inside.glb  shell and caps hidden, to see the layout
python3 machine.py [--no-check]
"""
import itertools, sys
import numpy as np
import trimesh
from trimesh.visual.material import PBRMaterial
from PIL import Image
from build123d import *
import params as M
from params import place
import shell, base, jog, controls, stock

LOOK = {   # rgba, metallic, roughness, emissive
    "shell":   ((0.86, 0.87, 0.88, 1), 0, 0.55, None),     # light grey PETG
    "base":    ((0.12, 0.12, 0.13, 1), 0, 0.6, None),
    "platter": ((0.05, 0.05, 0.055, 1), 0, 0.42, None),
    "collar":  ((0.9, 0.45, 0.1, 1), 0, 0.5, None),
    "btn":     ((0.06, 0.06, 0.065, 1), 0, 0.55, None),
    "icon_cue": ((1.0, 0.6, 0.2, 1), 0, 0.5, None),
    "icon_play": ((0.3, 0.95, 0.5, 1), 0, 0.5, None),
    "knob":    ((0.06, 0.06, 0.065, 1), 0, 0.45, None),
    "white":   ((0.95, 0.95, 0.95, 1), 0, 0.5, None),
    "steel":   ((0.62, 0.63, 0.65, 1), 1, 0.3, None),
    "magnet":  ((0.35, 0.35, 0.37, 1), 1, 0.4, None),
    "pcb":     ((0.05, 0.22, 0.12, 1), 0, 0.5, None),
    "pcb_blue": ((0.06, 0.12, 0.45, 1), 0, 0.5, None),
    "pcb_purple": ((0.25, 0.08, 0.35, 1), 0, 0.5, None),
    "chip":    ((0.02, 0.02, 0.02, 1), 0, 0.4, None),
    "glass":   ((0.01, 0.01, 0.012, 1), 0, 0.08, None),
    "batt":    ((0.75, 0.76, 0.78, 1), 0.6, 0.35, None),
    "jack":    ((0.03, 0.03, 0.03, 1), 0, 0.6, None),
    "tact":    ((0.15, 0.15, 0.16, 1), 0, 0.5, None),
    "pot":     ((0.55, 0.56, 0.58, 1), 0.8, 0.35, None),
}
CUT_LOOK = {"shell": ((0.55, 0.57, 0.6, 1), 0, 0.6, None), "base": ((0.30, 0.31, 0.33, 1), 0, 0.6, None),
            "platter": ((0.18, 0.18, 0.2, 1), 0, 0.5, None), "magnet": ((0.8, 0.2, 0.2, 1), 0, 0.4, None),
            "steel": ((0.55, 0.65, 0.85, 1), 0.4, 0.4, None)}

def scene_parts():
    S = shell.MATES
    out = []
    add = lambda n, s, lk: out.append((n, s, lk))
    add("shell", shell.part(), "shell")
    add("base", place(base.part(), base.MATES["to_shell"], S["base"]), "base")
    axis = Pos(*M.JOG_C, 0)
    for key, fn, lk in (("platter", jog.platter, "platter"), ("bearing_top", jog.bearing, "steel"),
                        ("bearing_bot", jog.bearing, "steel"), ("collar", jog.collar, "collar"),
                        ("magnet", jog.magnet, "magnet"), ("encoder_pcb", jog.encoder_pcb, "pcb_purple"),
                        ("encoder_chip", jog.encoder_chip, "chip")):
        add(key, place(fn(), jog.MATES["seat"], axis * Pos(0, 0, jog.STACK[key])), lk)
    for which in ("cue", "play"):
        P = lambda s: place(s, controls.MATES["seat"], S[which])
        add(f"{which}_cap", P(controls.button_cap(which)), "btn")
        add(f"{which}_icon", P(controls.icon_fill(which)), f"icon_{which}")
        add(f"{which}_tact", Pos(*getattr(M, which.upper() + "_C"), M.MAIN_TOP) * controls.tact(), "tact")
    add("pot", place(controls.pot(M.KNOB_Y - M.POT_C), controls.MATES["seat"], S["pot"]), "pot")
    PK = lambda s: place(s, controls.MATES["seat"], S["knob"])
    add("knob", PK(controls.knob()), "knob")
    add("knob_line", PK(controls.knob_line()), "white")
    add("main_pcb", stock.main_pcb(), "pcb")
    xp, xs = stock.xiao()
    add("xiao_pcb", xp, "pcb_blue"); add("xiao_shield", xs, "steel"); add("usb_c", stock.usb_c(), "steel")
    sp, sg, spins = stock.screen()
    add("screen_pcb", sp, "pcb_blue"); add("screen_glass", sg, "glass"); add("screen_pins", spins, "steel")
    add("battery", stock.battery(), "batt")
    add("jack", stock.jack(), "jack")
    add("dac", stock.dac(), "chip")
    return out

# pairs that are meant to overlap (press fits, solder joints through boards)
ALLOWED = {("pot", "main_pcb"), ("knob", "knob_line")}   # pot pins solder through; the line is an inlay

def check(parts):
    """Every pair whose bounding boxes overlap: intersect and report real overlap."""
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

def screen_mesh():
    aw, ah = M.SCR_ACTIVE
    z = M.GLASS_TOP + 0.02
    cx, cy = M.WIN_C
    pts = [(cx - aw / 2, cy - ah / 2, z), (cx + aw / 2, cy - ah / 2, z), (cx + aw / 2, cy + ah / 2, z), (cx - aw / 2, cy + ah / 2, z)]
    img = Image.open(M.out("screen.png")).convert("RGB")
    mat = PBRMaterial(name="screen", baseColorTexture=img, emissiveTexture=img, emissiveFactor=[1, 1, 1],
                      metallicFactor=0, roughnessFactor=0.2)
    return trimesh.Trimesh(vertices=np.array(pts), faces=[[0, 1, 2], [0, 2, 3]],
                           visual=trimesh.visual.TextureVisuals(uv=np.array([(0, 0), (1, 0), (1, 1), (0, 1)]), material=mat),
                           process=False)

def export(parts, path, with_screen=True, look=None):
    look = {**LOOK, **(look or {})}
    scene = trimesh.Scene()
    for name, solid, lk in parts:
        f = M.out(f"viz/{name}.stl")
        export_stl(solid, f, tolerance=0.01, angular_tolerance=0.05)
        # STL has no normals and trimesh welds its vertices, so a viewer would smooth every edge
        # (holes turn into dimples). Split the mesh at creases and write the normals out.
        m = trimesh.graph.smooth_shade(trimesh.load(f), angle=np.radians(30))
        rgba, metal, rough, emis = look[lk]
        kw = dict(name=lk, baseColorFactor=rgba, metallicFactor=metal, roughnessFactor=rough,
                  alphaMode="BLEND" if rgba[3] < 1 else "OPAQUE", doubleSided=True)
        if emis: kw["emissiveFactor"] = list(emis)
        m.visual = trimesh.visual.TextureVisuals(material=PBRMaterial(**kw))
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
    keep = Pos(M.JOG_C[0], M.D / 2, 0) * Box(400, 400, 200, align=(Align.MAX, Align.CENTER, Align.CENTER))
    cut = []
    for name, s, lk in parts:
        bb = s.bounding_box()
        if bb.min.X >= M.JOG_C[0]: continue
        if bb.max.X <= M.JOG_C[0]: cut.append((name, s, lk)); continue
        c = s & keep
        if c.volume > 0.01: cut.append((name, c, lk))
    export(cut, M.out("deck_cut.glb"), with_screen=False, look=CUT_LOOK)
    hide = ("shell", "cue_cap", "play_cap", "cue_icon", "play_icon", "knob", "knob_line", "platter", "screen_pcb", "screen_glass", "screen_pins")
    export([p for p in parts if p[0] not in hide], M.out("deck_inside.glb"), with_screen=False)
    return ok

if __name__ == "__main__":
    sys.exit(0 if build("--no-check" not in sys.argv) else 1)
