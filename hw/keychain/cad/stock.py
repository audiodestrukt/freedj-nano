# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""stock.py: bought parts and the main PCB as simple stand-ins, already in world
coordinates (they only ever sit where params.py says). Used by machine.py for the
renders and the fit check, never printed.
"""
from build123d import *
import params as M

C = (Align.CENTER, Align.CENTER, Align.MIN)

def main_pcb():
    """The whole inside, less the encoder well and the pot pins' holes."""
    i = M.MAIN_INSET
    w, d = M.W - 2 * i, M.D - 2 * i
    p = Pos(M.W / 2, M.D / 2, M.MAIN_Z) * extrude(RectangleRounded(w, d, M.INNER_R), M.MAIN_T)
    return p - Pos(*M.JOG_C, M.MAIN_Z - 1) * Box(M.ENC_CUT, M.ENC_CUT, M.MAIN_T + 2, align=C)

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
    o = Pos(cx, mouth - dpt / 2, M.MAIN_TOP + M.XIAO[2]) * Box(w, dpt, h, align=C)
    return o - Pos(cx, mouth - dpt / 2 + 0.6, M.MAIN_TOP + M.XIAO[2] + 0.4) * Box(w - 0.8, dpt, h - 0.8, align=C)

def oled():
    """Board, glass and the active area (a thin slab the screen texture goes on)."""
    bx, by, bt = M.OLED_PCB
    pcb = Pos(*M.OLED_C, M.OLED_Z) * Box(bx, by, bt, align=C)
    hx, hy = M.OLED_HOLES
    pcb -= [Pos(M.OLED_C[0] + sx * hx / 2, M.OLED_C[1] + sy * hy / 2, M.OLED_Z - 1) * Cylinder(1.1, 4, align=C)
            for sx in (-1, 1) for sy in (-1, 1)]
    gx, gy, gt = M.OLED_GLASS
    glass = Pos(*M.GLASS_C, M.OLED_Z + bt) * Box(gx, gy, gt, align=C)
    pins = Pos(M.OLED_C[0], M.OLED_C[1] + (-1 if M.OLED_HEADER_FRONT else 1) * (by / 2 - 1.6), M.OLED_Z - 1.0) * Box(7 * 2.54, 1.2, 1.0, align=C)
    return pcb, glass, pins

def battery():
    x, y, z = M.BATT
    return Pos(*M.BATT_C, M.BASE_T - 0.6) * Box(x, y, z, align=C)

def jack():
    l, w, h = M.JACK
    x0 = M.W - M.WALL - 0.3 - l
    body = Pos(x0 + l / 2, M.JACK_C_Y, M.MAIN_TOP) * Box(l, w, h, align=C)
    bd, bl = M.JACK_BARREL
    barrel = Pos(x0 + l, M.JACK_C_Y, M.MAIN_TOP + h / 2) * Rot(0, 90, 0) * Cylinder(bd / 2, bl, align=C)
    hole = Pos(x0 + l - 8, M.JACK_C_Y, M.MAIN_TOP + h / 2) * Rot(0, 90, 0) * Cylinder(1.8, 12, align=C)
    return body.fuse(barrel) - hole

def dac():
    return Pos(*M.DAC_C, M.MAIN_TOP) * Box(6.5, 4.4, 1.2, align=C)
