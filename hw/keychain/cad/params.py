# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""params.py: every shared dimension of the keychain deck (mm), plus place().

Frame: x across the deck (0 = left edge), y front to back (0 = the edge nearest you),
z up (0 = underside of the base plate). Built around:
  - Seeed XIAO ESP32-S3 (USB host for a stick, out the back edge)
  - a generic 0.95" SSD1331 colour OLED (96 x 64, 7-pin SPI module)
  - a generic 35 mm slide pot with a T-shaft (pitch), down the right side
  - the AS5600 breakout freedj-esp uses (23.8 mm square), under a 32 mm jog
  - CUE / PLAY on 6 x 6 tact switches, a 402030 LiPo, a PCM5102A and a 3.5 mm jack
    on one main PCB (the XIAO sits on it).

Numbers marked MEASURE come from listings or typical parts, not from the parts Dan has:
check them with calipers before printing (and fix them here; everything follows).
"""
import math, os
from build123d import Location, Pos, Rot

OUT = "build"
def out(name):
    p = os.path.join(OUT, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p

# ---------------------------------------------------------------- body
WALL = 1.8               # (W, the width, follows from the layout below)
TOP_T = 1.6              # top plate
BASE_T = 1.5             # base plate
CORNER_R = 5.0           # plan-view corner radius, outside
INNER_R = 1.0            # inside corners: tight, so the fader frame can reach the front-right corner
EDGE_R = 1.2             # top-edge round

# ---------------------------------------------------------------- electronics stack (bottom up)
# 402030 LiPo (4 x 20 x 30, ~200 mAh) lies on the base under the screen, under the main PCB.
BATT = (30.0, 20.0, 4.0)             # x, y, z. MEASURE (pouch cells swell: 0.4 slack above)
MAIN_T = 1.6
MAIN_Z = BASE_T + BATT[2] + 0.4      # main PCB underside: 5.9
MAIN_TOP = MAIN_Z + MAIN_T           # 7.5
# XIAO ESP32-S3, soldered flat (castellated) on the main PCB, USB-C out the back edge.
XIAO = (17.8, 21.0, 1.0)             # x, y, PCB thickness (Seeed: 21 x 17.8)
XIAO_H = 3.3                         # tallest part (USB-C, shield) above its PCB. MEASURE
XIAO_TOP = MAIN_TOP + XIAO[2] + XIAO_H
USBC = (8.9, 3.2, 7.3)               # receptacle w, h, depth; mouth USBC_OVER past the XIAO's edge
USBC_OVER = 1.3
# OLED module above the XIAO, glass up against the top plate's underside.
OLED_PCB = (30.5, 27.5, 1.2)         # generic 7-pin SSD1331 board. MEASURE
OLED_HOLES = (27.0, 23.0)            # M2 x 4, from a listing. MEASURE
OLED_GLASS = (25.7, 17.6, 1.5)       # panel + tape. MEASURE
OLED_GLASS_DY = 0.0                  # glass centre from the board centre, toward the back. MEASURE
OLED_ACTIVE = (20.14, 13.42)         # 96 x 64 at 0.21 mm (SSD1331 panel drawing)
OLED_AA_DY = 0.0                     # active area from the glass centre. MEASURE
OLED_HEADER_FRONT = True             # 7-pin header on the edge toward the jog (the back edge is over the USB-C)
OLED_CLEAR = 0.5                     # XIAO top to OLED board underside
GASKET = 0.3                         # foam/tape between glass and top
OLED_Z = XIAO_TOP + OLED_CLEAR       # OLED board underside: 12.3
GLASS_TOP = OLED_Z + OLED_PCB[2] + OLED_GLASS[2]
H = GLASS_TOP + GASKET + TOP_T       # 16.9: the screen stack sets the height
TOP_UNDER = H - TOP_T

# ---------------------------------------------------------------- jog
JOG_D = 32.0
WELL_D = JOG_D + 2.5
WELL_DEPTH = 0.8
FLOOR = H - WELL_DEPTH
PLATTER_H = 4.5
PLATTER_GAP = 0.6                    # platter underside above the well floor
# Two MR106ZZ (6 x 10 x 3) in a boss under the well, a 1 mm ledge between them.
BRG = (6.0, 10.0, 3.0)
BRG_LEDGE = 1.0
BRG_INNER_OD = 7.2                   # inner race OD: hub and collar stay inside it
BOSS_WALL = 1.6
BOSS_OD = BRG[1] + 2 * BOSS_WALL
BOSS_BOTTOM = FLOOR - 2 * BRG[2] - BRG_LEDGE
# Shaft printed with the platter (6 mm, tune on a fit gauge), magnet flush in its tip.
# A printed collar glued on under the lower bearing holds it in.
SHAFT_D = 6.0
COLLAR = (BRG_INNER_OD, 1.5)         # OD, height
SHAFT_PAST = COLLAR[1] + 0.5         # shaft below the lower bearing
SHAFT_BOTTOM = BOSS_BOTTOM - SHAFT_PAST
MAGNET = (4.0, 2.0)                  # diametric, same as freedj-esp
MAGNET_FIT = 0.15
AIR_GAP = 1.0                        # magnet to chip
ENC_PCB = (23.8, 23.8, 1.6)          # AS5600 breakout, chip side up, wires soldered (no headers)
ENC_CHIP_H = 1.5
_h = 15.875 / 2
ENC_HOLES = [(-_h, -_h), (_h, -_h), (-_h, _h), (_h, _h)]
ENC_Z = SHAFT_BOTTOM - AIR_GAP - ENC_CHIP_H - ENC_PCB[2]   # board underside
ENC_STANDOFF = ENC_Z - BASE_T
assert ENC_STANDOFF >= 1.0, ("encoder board too low: raise H", ENC_STANDOFF)
assert H - WELL_DEPTH - TOP_UNDER >= 0.6, "top too thin under the jog well"

# ---------------------------------------------------------------- CUE / PLAY
# Caps sized to the jog: freedj-esp is 22 mm buttons on a 100 mm jog; here 8 on 32.
BTN_D = 8.0
BTN_HOLE = BTN_D + 0.4
BTN_RISE = 1.5                       # above the top
BTN_FLANGE = (10.0, 0.8)             # under the top: stops the cap coming out
BTN_GAP = 0.2                        # flange below the top at rest
NUB_D = 3.0
TACT = (6.0, 6.0, 3.5, 5.0)          # 6 x 6 SMD tact: body x, y, body h, actuator top above the PCB
# CDJ layout: CUE above PLAY, stacked in the bottom-left corner, the jog beside them.
BTN_EDGE = 1.6                       # hole edge to the inside of the wall
BTN_PITCH = BTN_FLANGE[0] + 0.6      # flanges 0.6 apart
PLAY_C = (WALL + BTN_EDGE + BTN_HOLE / 2, WALL + BTN_EDGE + BTN_HOLE / 2)
CUE_C = (PLAY_C[0], PLAY_C[1] + BTN_PITCH)
# Jog: its front edge PLATTER_PLAY_DY behind the front of the PLAY cap (CDJ: the platter's
# bottom lines up with PLAY's), and as far left as CUE allows (hole BTN_WELL_GAP from the well).
PLATTER_PLAY_DY = 0.5
JOG_Y = PLAY_C[1] - BTN_D / 2 + PLATTER_PLAY_DY + JOG_D / 2
BTN_WELL_GAP = 1.2
_r = WELL_D / 2 + BTN_HOLE / 2 + BTN_WELL_GAP
JOG_C = (CUE_C[0] + math.sqrt(_r ** 2 - (JOG_Y - CUE_C[1]) ** 2), JOG_Y)

# ---------------------------------------------------------------- pitch fader
# Generic 35 mm travel slide pot, T-shaft lever. Frame = travel + 15 like the 45 / 60 mm
# ones (PTA4543 60 x 9, holes 56; PTA6043 75 x 9, holes 71), so 50 x 9, holes 46.
POT_TRAVEL = 35.0
POT_FRAME = (9.0, 50.0, 6.5)         # across, along, frame top -> body bottom. MEASURE
POT_HOLES = 46.0                     # M2 in the frame top. MEASURE (some have tabs instead)
POT_PINS_L = 3.5                     # PC pins below the body, soldered through the main PCB
LEVER = (1.2, 4.0, 10.0)             # across, along, above the frame top. MEASURE
KNOB = (8.0, 6.5)                    # across, along
POT_X = JOG_C[0] + JOG_D / 2 + 3.0 + KNOB[0] / 2   # knob 3 mm clear of the platter
W = round(POT_X + POT_FRAME[0] / 2 + 0.5 + WALL, 1)
POT_FRONT_GAP = 0.3                  # frame's front end to the inside of the front wall
POT_C = WALL + POT_FRONT_GAP + POT_FRAME[1] / 2   # fader as far forward as it goes
POT_TOP = TOP_UNDER                  # frame top up against the top plate
SLOT_W = LEVER[0] + 1.2
SLOT_L = POT_TRAVEL + LEVER[1] + 0.6
SLIDER_Y = (POT_C - SLOT_L / 2, POT_C + SLOT_L / 2)
KNOB_LIFT = 0.8
KNOB_H = LEVER[2] - TOP_T - KNOB_LIFT + 1.0   # the lever stops 1 mm under the knob's top
KNOB_SOCKET = (LEVER[0] + 0.3, LEVER[1] + 0.3)
KNOB_Y = POT_C - 4.0                 # where the render puts it
assert POT_TOP - POT_FRAME[2] > MAIN_TOP + 0.5, "pot body sits on the main PCB's parts"
assert POT_TOP - POT_FRAME[2] - POT_PINS_L < MAIN_Z, "pot pins don't reach through the main PCB"
assert JOG_C[0] + ENC_PCB[0] / 2 + 0.5 < POT_X - POT_FRAME[0] / 2, "encoder board hits the pot frame"
assert JOG_C[0] - WELL_D / 2 > WALL + 1.0, "jog well too close to the left wall"
assert JOG_C[1] - WELL_D / 2 > WALL + 1.0, "jog well too close to the front wall"

# ---------------------------------------------------------------- screen position
WINDOW_MARGIN = 0.5
WINDOW_LIP = 0.6                     # straight part of the window wall; a 45 degree chamfer above
WIN = (OLED_ACTIVE[0] + 2 * WINDOW_MARGIN, OLED_ACTIVE[1] + 2 * WINDOW_MARGIN)
_well_back = JOG_C[1] + WELL_D / 2
WIN_C = (JOG_C[0], _well_back + 3.0 + WIN[1] / 2)            # window over the jog, 3 mm behind the well
GLASS_C = (WIN_C[0], WIN_C[1] - OLED_AA_DY)
OLED_C = (GLASS_C[0], GLASS_C[1] - OLED_GLASS_DY)
D = round(OLED_C[1] + OLED_PCB[1] / 2 + 0.5 + WALL, 1)      # back edge just behind the OLED board
OLED_BOSS_D = 4.2
OLED_PILOT = (1.6, 1.6)              # M2 thread-former: pilot dia, depth into the boss

# XIAO under the OLED, USB-C at the back
XIAO_C = (JOG_C[0], D - WALL - 0.2 - XIAO[1] / 2)
BATT_C = (JOG_C[0], XIAO_C[1] - 1.0)
DAC_C = (XIAO_C[0] + XIAO[0] / 2 + 4.5, XIAO_C[1] - 6.0)   # right of the XIAO, in front of the jack; PCM5102A (TSSOP-20)
# 3.5 mm jack (PJ-320D style SMD) on the main PCB, out the right wall behind the fader
JACK = (12.0, 6.0, 5.0)              # length (x), width (y), height. MEASURE
JACK_BARREL = (5.0, 2.0)             # dia, length past the body into the wall
JACK_C_Y = POT_C + POT_FRAME[1] / 2 + 1.2 + JACK[1] / 2
JACK_HOLE = 6.0
assert JACK_C_Y + JACK[1] / 2 < D - WALL, "jack hits the back wall"

# Base fixing: four short corner bosses fused into the walls, M2 thread-formers from below
POST_D = 5.0
POST_INSET = 3.9
POST_TOP = MAIN_Z - 0.3              # under the main PCB, so the PCB needs no notches
POST_PILOT = (1.6, 3.5)              # M2 thread-former pilot, up from the base (M2 x 5 countersunk)

# Keyring tab off the back-left corner, flush with the top (prints on the bed)
KEYRING = False                      # keyring tab off the back-left corner (left off for now)
TAB = (9.0, 7.0, 3.5)                # width, reach past the back edge, thickness
TAB_X = 8.0
TAB_HOLE = 4.5

# Main PCB: the whole inside, less the encoder well
MAIN_INSET = WALL + 0.3
ENC_CUT = ENC_PCB[0] + 1.6

def place(solid, frm: Location, onto: Location):
    """Snap a part so its mate `frm` (local frame) lands on world `onto`."""
    return (onto * frm.inverse()) * solid

if __name__ == "__main__":
    print(f"keychain deck {W} x {D} x {H:.1f} mm" + (f" (+{TAB[1]} tab)" if KEYRING else "") + f", platter top at {FLOOR + PLATTER_GAP + PLATTER_H:.1f}")
    print(f"  jog: floor {FLOOR:.1f}, boss {BOSS_BOTTOM:.1f}, shaft tip {SHAFT_BOTTOM:.1f}, encoder board on {ENC_STANDOFF:.2f} mm bosses")
    print(f"  stack: main PCB {MAIN_Z:.1f}-{MAIN_TOP:.1f}, XIAO top {XIAO_TOP:.1f}, OLED {OLED_Z:.1f}, glass top {GLASS_TOP:.1f}")
    print(f"  pot: frame bottom {POT_TOP - POT_FRAME[2]:.1f}, knob {KNOB_H:.1f} tall, slot {SLOT_L:.1f}")
