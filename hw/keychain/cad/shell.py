# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""shell.py: the keychain deck's top shell. One print, upside down on the bed, no supports
(the keyring tab is flush with the top, the bosses hang off the top plate).

Carries the jog well and the MR106 bearing boss, the CUE / PLAY holes, the fader slot with
countersunk M2s into the pot frame, the screen window (chamfered) and its four bosses, the
USB-C and headphone-jack openings, four short corner bosses for the base, and the keyring tab.
python3 shell.py -> build/shell.stl
"""
from build123d import *
import params as M

C = (Align.CENTER, Align.CENTER, Align.MIN)

def posts(webs=False):
    """Base screw bosses: three corners, and the front-right one moved left of the fader
    (its front end and pins come right down to the front wall). With webs=True, also the
    walls each one is tied to: "l", "r", "f", "b"."""
    i = M.POST_INSET
    fr = M.POT_X - M.POT_FRAME[0] / 2 - M.POST_D / 2 - 0.5
    p = [(i, i, "lf"), (fr, i, "f"), (i, M.D - i, "lb"), (M.W - i, M.D - i, "rb")]
    return p if webs else [(x, y) for x, y, _ in p]

def _web(x, y, side, h):
    """A post's web out to one wall (clipped to the inside later)."""
    d = M.POST_D
    return {"l": Pos(x / 2, y, M.BASE_T) * Box(x, d, h, align=C),
            "r": Pos((x + M.W) / 2, y, M.BASE_T) * Box(M.W - x, d, h, align=C),
            "f": Pos(x, y / 2, M.BASE_T) * Box(d, y, h, align=C),
            "b": Pos(x, (y + M.D) / 2, M.BASE_T) * Box(d, M.D - y, h, align=C)}[side]

def screen_holes():
    hx, hy = M.SCR_HOLES
    return [(M.SCR_C[0] + sx * hx / 2, M.SCR_C[1] + sy * hy / 2) for sx in (-1, 1) for sy in (-1, 1)]

def _plan(inset, z0, h):
    r = M.CORNER_R if inset == 0 else M.INNER_R + M.WALL - inset   # walls thicken into the corners
    return Pos(M.W / 2, M.D / 2, z0) * extrude(RectangleRounded(M.W - 2 * inset, M.D - 2 * inset, r), h)

def tab():
    tw, reach, tt = M.TAB
    y0 = M.D - M.CORNER_R
    body = Pos(M.TAB_X, (y0 + M.D + reach - tw / 2) / 2, M.H - tt) * Box(tw, M.D + reach - tw / 2 - y0, tt, align=C)
    body = body.fuse(Pos(M.TAB_X, M.D + reach - tw / 2, M.H - tt) * Cylinder(tw / 2, tt, align=C))
    return body - Pos(M.TAB_X, M.D + reach - tw / 2, M.H - tt - 1) * Cylinder(M.TAB_HOLE / 2, tt + 2, align=C)

def part():
    outer = _plan(0, 0, M.H)
    try:
        outer = fillet(outer.edges().filter_by(lambda e: e.center().Z > M.H - 0.05), M.EDGE_R)
    except Exception as e:
        print("  (top-edge round skipped:", e, ")")
    shell = outer - _plan(M.WALL, -1, M.TOP_UNDER + 1)
    jx, jy = M.JOG_C
    inside = _plan(M.WALL - 0.01, 0, M.H)
    adds = [Pos(jx, jy, M.BOSS_BOTTOM) * Cylinder(M.BOSS_OD / 2, M.TOP_UNDER - M.BOSS_BOTTOM + 0.5, align=C)]
    ph = M.POST_TOP - M.BASE_T
    for x, y, walls in posts(webs=True):   # short bosses, webbed into the walls
        adds.append(Pos(x, y, M.BASE_T) * Cylinder(M.POST_D / 2, ph, align=C))
        adds += [_web(x, y, s, ph) for s in walls]
    for s in (-1, 1):   # fader pads: the frame hangs from these
        if M.POT_PAD > 0:
            adds.append(Pos(M.POT_X, M.POT_C + s * M.POT_HOLES / 2, M.POT_TOP) *
                        Box(M.POT_FRAME[0] - 1.0, M.POT_PAD_L, M.POT_PAD + 0.5, align=C))
    scr_pcb_top = M.SCR_Z + M.SCR_PCB[2]
    for x, y in screen_holes():   # screen bosses: down from the top to the module's board
        adds.append(Pos(x, y, scr_pcb_top) * Cylinder(M.SCR_BOSS_D / 2, M.TOP_UNDER - scr_pcb_top + 0.5, align=C))
    shell = shell.fuse(*[a & inside for a in adds], *([tab() - _plan(M.WALL, -1, M.TOP_UNDER + 1)] if M.KEYRING else []))

    cuts = []
    cuts.append(Pos(jx, jy, M.FLOOR) * Cylinder(M.WELL_D / 2, 10, align=C))
    b_id, b_od, b_w = M.BRG
    cuts.append(Pos(jx, jy, M.FLOOR - b_w) * Cylinder(b_od / 2 + 0.05, b_w + 1, align=C))       # upper bearing
    cuts.append(Pos(jx, jy, M.BOSS_BOTTOM - 1) * Cylinder(b_od / 2 + 0.05, b_w + 1, align=C))   # lower bearing
    cuts.append(Pos(jx, jy, 0) * Cylinder(b_od / 2 - 1.0, 30, align=C))                         # the ledge between
    for c in (M.CUE_C, M.PLAY_C):
        cuts.append(Pos(*c, M.TOP_UNDER - 1) * Cylinder(M.BTN_HOLE / 2, M.TOP_T + 2, align=C))
    y0, y1 = M.SLIDER_Y
    cuts.append(Pos(M.POT_X, M.POT_C, M.TOP_UNDER - 1) * Box(M.SLOT_W, y1 - y0, M.TOP_T + 2, align=C))
    for s in (-1, 1):   # M2 countersunk into the pot frame's tapped holes
        y = M.POT_C + s * M.POT_HOLES / 2
        cuts.append(Pos(M.POT_X, y, M.POT_TOP - 1) * Cylinder(1.15, M.TOP_T + M.POT_PAD + 2, align=C))
        cuts.append(Pos(M.POT_X, y, M.H - 0.95) * Cone(1.15, 2.05, 0.96, align=C))
    for k in range(-4, 5):   # tempo ticks left of the knob, engraved 0.4 deep
        ln = 2.0 if k in (-4, 0, 4) else 1.2
        x1 = M.POT_X - M.KNOB[0] / 2 - 0.6
        cuts.append(Pos(x1 - ln / 2, M.POT_C + k * M.POT_TRAVEL / 8, M.H - 0.4) * Box(ln, 0.5, 1, align=C))
    # screen window: straight lip, then 45 degrees out to the top
    ww, wh = M.WIN
    ch = M.TOP_T - M.WINDOW_LIP
    cuts.append(Pos(*M.WIN_C, M.TOP_UNDER - 1) * Box(ww, wh, M.TOP_T + 2, align=C))
    cuts.append(Pos(*M.WIN_C, M.H - ch) * extrude(Rectangle(ww, wh), amount=ch + 0.01, taper=-45))
    cuts.append(Pos(*M.WIN_C, M.H) * Box(ww + 2 * ch, wh + 2 * ch, 2, align=C))
    # clearance over the glass (a pocket in the bosses' way would be wrong: the glass is narrower)
    for x, y in screen_holes():
        cuts.append(Pos(x, y, M.SCR_Z + M.SCR_PCB[2] - 0.01) * Cylinder(M.SCR_PILOT[0] / 2, M.SCR_PILOT[1] + 0.01, align=C))
    # USB-C through the back wall, with a relief round it for a stick's overmould
    xz = M.MAIN_TOP + M.XIAO[2] + M.USBC[1] / 2
    cuts.append(Pos(M.XIAO_C[0], M.D - M.WALL / 2, xz) * Box(M.USBC[0] + 0.8, M.WALL + 2, M.USBC[1] + 0.8))
    cuts.append(Pos(M.XIAO_C[0], M.D, xz) * Box(12.6, 1.4, 7.0))
    # headphone jack through the side wall
    jx0 = M.W - M.WALL - 1 if M.JACK_SIDE == "right" else -2
    cuts.append(Pos(jx0, M.JACK_C_Y, M.MAIN_TOP + M.JACK[2] / 2) * Rot(0, 90, 0) * Cylinder(M.JACK_HOLE / 2, M.WALL + 4, align=C))
    for x, y in posts():
        cuts.append(Pos(x, y, M.BASE_T - 0.01) * Cylinder(M.POST_PILOT[0] / 2, M.POST_PILOT[1] + 0.01, align=C))   # up from the base
    try:   # brand: behind the window if there's room (OLED), else up the strip left of it (2")
        txt = Text("FREEDJ", 2.6, font_style=FontStyle.BOLD, align=(Align.CENTER, Align.CENTER))
        back = M.D - M.WALL - (M.WIN_C[1] + wh / 2 + ch)
        if back >= 3.4:
            at = Pos(M.WIN_C[0], (M.WIN_C[1] + wh / 2 + ch + M.D - M.WALL) / 2, M.H - 0.4)
        else:
            left = M.WIN_C[0] - ww / 2 - ch
            assert left - M.WALL >= 4.0, "no room for the brand"
            at = Pos((M.WALL + left) / 2, M.WIN_C[1], M.H - 0.4) * Rot(0, 0, 90)
        cuts.append(at * extrude(txt, 1))
    except Exception as e:
        print("  (brand skipped:", e, ")")
    return shell.cut(*cuts)

MATES = {   # world frame; +z out of the contact face
    "jog":    Pos(*M.JOG_C, M.FLOOR),
    "cue":    Pos(*M.CUE_C, M.H),
    "play":   Pos(*M.PLAY_C, M.H),
    "pot":    Pos(M.POT_X, M.POT_C, M.POT_TOP),        # frame top, slot centre
    "knob":   Pos(M.POT_X, M.KNOB_Y, M.H),
    "glass":  Pos(*M.GLASS_C, M.GLASS_TOP),
    "base":   Pos(0, 0, 0),
}

if __name__ == "__main__":
    p = part()
    f = M.out("shell.stl")
    export_stl(p, f, tolerance=0.03, angular_tolerance=0.1)
    import trimesh; m = trimesh.load(f)
    print(f, (m.bounds[1] - m.bounds[0]).round(1), "bodies:", len(m.split(only_watertight=False)), "watertight:", m.is_watertight)
