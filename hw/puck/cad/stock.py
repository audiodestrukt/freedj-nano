# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""stock.py: bought parts and the main PCB as simple stand-ins, in world coordinates (they
only ever sit where params.py says). Used by machine.py for the renders and the fit check.
"""
from build123d import *
import params as M
from jog import tube

C = (Align.CENTER, Align.CENTER, Align.MIN)

def screen():
    """Waveshare 1.28inch Touch LCD: (lens, LCD module, board with its standoffs and connector)."""
    lens = Pos(0, 0, M.GLASS_TOP - M.LENS_T) * Cylinder(M.LENS_D / 2, M.LENS_T, align=C)
    lcm = Pos(0, 0, M.GLASS_TOP - M.LENS_T - M.LCM_T) * Cylinder(M.LCM_D / 2, M.LCM_T, align=C)
    bx, by = M.SCR_PCB_BOX
    z = M.SCR_PCB_Z
    pcb = Pos(0, M.SCR_PCB_DY, z) * Box(bx, by, M.SCR_PCB_T, align=C) & Pos(0, M.SCR_PCB_DY, z) * Cylinder(M.SCR_PCB_R, M.SCR_PCB_T, align=C)
    sd, sh = M.SCR_STANDOFF
    hx, hy = M.SCR_HOLES
    feet = [Pos(sx * hx / 2, sy * hy / 2, z - sh) * Cylinder(sd / 2, sh + 0.01, align=C) for sx in (-1, 1) for sy in (-1, 1)]
    feet = [f - Pos(f.center().X, f.center().Y, z - sh - 1) * Cylinder(0.8, sh + 1, align=C) for f in feet]
    cx, cy, cd, coff = M.SCR_CONN
    conn = Pos(0, coff, z - cd) * Box(cx, cy, cd + 0.01, align=C)
    bd, bh = M.SCR_BACK_PARTS
    back = Pos(0, 0, z - bh) * Cylinder(bd / 2 - 3, bh + 0.01, align=C)
    return lens, lcm, pcb.fuse(*feet, conn, back)

def bearing():
    d, D, B = M.BRG
    inner = tube(M.BRG_IN_LAND, d, M.BRG_Z, M.BRG_TOP)
    outer = tube(D, M.BRG_OUT_LAND, M.BRG_Z, M.BRG_TOP)
    seal = tube(M.BRG_OUT_LAND + 0.01, M.BRG_IN_LAND - 0.01, M.BRG_Z + 0.3, M.BRG_TOP - 0.3)
    return inner.fuse(outer, seal)

def sensor(a):
    """Slotted opto-interrupter, slot across the comb, on its leads above the main PCB."""
    z0 = M.SENS_TOP - M.SENS_H
    body = Pos(M.SENS_R, 0, z0) * Box(M.SENS_L, M.SENS_W, M.SENS_H, align=C)
    body -= Pos(M.SENS_R, 0, M.SENS_TOP - M.SENS_SLOT_D) * Box(M.SENS_GAP, M.SENS_W + 2, M.SENS_SLOT_D + 1, align=C)
    legs = [Pos(M.SENS_R + sx * 5.0, sy * 1.27, M.MAIN_Z) * Box(0.5, 0.5, z0 - M.MAIN_Z + 0.01, align=C)
            for sx in (-1, 1) for sy in (-1, 1)]
    return Rot(0, 0, a) * body.fuse(*legs)

def main_pcb():
    p = Pos(0, 0, M.MAIN_Z) * Cylinder(M.R_IN - 0.3, M.MAIN_T, align=C)
    return p.cut(*[Pos(*M.ang(a, M.BOSS_R), M.MAIN_Z - 1) * Cylinder(1.2, M.MAIN_T + 2, align=C) for a in M.BOSS_ANG])

def xiao():
    x, y, t = M.XIAO
    cx, cy = M.XIAO_C
    pcb = Pos(cx, cy, M.MAIN_TOP) * Box(x, y, t, align=C)
    shield = Pos(cx, cy - 2.5, M.MAIN_TOP + t) * Box(12.5, 13.0, 2.4, align=C)
    return pcb, shield

def usb_c():
    w, h, dpt = M.USBC
    cx, cy = M.XIAO_C
    mouth = cy + M.XIAO[1] / 2 + M.USBC_OVER
    z = M.MAIN_TOP + M.XIAO[2]
    o = Pos(cx, mouth - dpt / 2, z) * Box(w, dpt, h, align=C)
    return o - Pos(cx, mouth - dpt / 2 + 0.6, z + 0.4) * Box(w - 0.8, dpt, h - 0.8, align=C)

def battery():
    x, y, z = M.BATT
    return Pos(*M.BATT_C, M.MAIN_TOP) * Box(x, y, z, align=C)

def jack():
    l, w, h = M.JACK
    bd, bl = M.JACK_BARREL
    x0 = M.JACK_X0
    body = Pos(x0 + l / 2, M.JACK_Y, M.MAIN_TOP) * Box(l, w, h, align=C)
    zc = M.MAIN_TOP + h / 2
    barrel = Pos(x0 - bl, M.JACK_Y, zc) * Rot(0, 90, 0) * Cylinder(bd / 2, bl + 0.01, align=(Align.CENTER, Align.CENTER, Align.MIN))
    hole = Pos(x0 - bl - 1, M.JACK_Y, zc) * Rot(0, 90, 0) * Cylinder(1.8, 10, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return body.fuse(barrel) - hole

def dac():
    return Pos(*M.DAC_C, M.MAIN_TOP) * Box(6.5, 4.4, 1.2, align=C)
