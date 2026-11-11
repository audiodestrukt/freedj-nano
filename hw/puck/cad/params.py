# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""params.py: every shared dimension of the puck deck (mm).

A round deck: a 1.28" round touch screen in the middle, and the whole top of the puck (a ring
round the glass plus the knurled side) is the jog. It turns on one 61808 thin-section bearing
(40 x 52 x 7) whose inner race sits on a printed post; a toothed comb under the jog runs
through two slotted opto-interrupters (quadrature), DJ2GO-style. Everything else is on the
touch screen.

Frame: x, y across the puck (0, 0 = the jog axis; -y is the front, toward you), z up
(0 = the underside). Built around:
  - Waveshare 1.28inch Touch LCD (GC9A01 240 x 240 + CST816S touch, 13-pin GH1.25), numbers
    from its 2D drawing and 3D model
  - one 61808 bearing, two ITR9608-style slotted opto-interrupters
  - Seeed XIAO ESP32-S3 (USB host for a stick, out the back), a 402030 LiPo, a PCM5102A and
    a 3.5 mm jack on one round main PCB under the deck plate.

Numbers marked MEASURE come from listings or typical parts, not from the parts Dan has:
check them with calipers before printing (and fix them here; everything follows).
"""
import math, os

OUT = "build"
def out(name):
    p = os.path.join(OUT, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p

def ang(deg, r):
    a = math.radians(deg)
    return (r * math.cos(a), r * math.sin(a))

# ---------------------------------------------------------------- screen (Waveshare drawing)
LENS_D = 38.51           # round cover glass (touch panel)
LENS_T = 1.85            # touch panel thickness
LENS_VA = 33.4           # black border inside this
ACTIVE_D = 32.4          # the 240 x 240 picture, a circle
LCM_D = 35.67            # LCD module under the lens
LCM_T = 1.55
SCR_PCB_T = 1.0
SCR_PCB_BOX = (37.02, 39.15)          # board outline: this box, corners clipped by SCR_PCB_R
SCR_PCB_R = 21.7
SCR_PCB_DY = -0.9                     # board centre is 0.9 mm off the glass centre (toward -y)
GLASS_TO_PCB = 4.0                    # glass face to the board's top
SCR_HOLES = (26.5, 18.65)             # 4 SMT M2 standoffs on the back
SCR_STANDOFF = (3.5, 2.5)             # dia, below the board
SCR_CONN = (19.5, 4.0, 4.3, 6.8)      # GH1.25 13-pin: x, y, depth below the board, y offset
SCR_BACK_PARTS = (35.0, 1.0)          # the other parts on the back: inside this dia, this tall
SCR_STACK = GLASS_TO_PCB + SCR_PCB_T + SCR_STANDOFF[1]   # glass face to standoff feet: 7.5
_hr = math.hypot(SCR_HOLES[0] / 2, SCR_HOLES[1] / 2)     # 16.2: screws sit at this radius
SCR_HOLE_ANG = [math.degrees(math.atan2(sy * SCR_HOLES[1] / 2, sx * SCR_HOLES[0] / 2))
                for sx in (1, -1) for sy in (1, -1)]
SCR_HOLE_R = _hr

# ---------------------------------------------------------------- bearing
BRG = (40.0, 52.0, 7.0)               # 61808: bore, OD, width
BRG_IN_LAND = 43.2                    # inner race OD (where the washer can press). MEASURE
BRG_OUT_LAND = 48.8                   # outer race ID (the face presses outside this). MEASURE
BRG_FIT = 0.0                         # added to the printed bore for the outer race (press)

# ---------------------------------------------------------------- opto-interrupters
# ITR9608 (Everlight): 13 long x 6 wide x 10 tall, 5 mm slot, optical axis assumed 3 mm under
# the top. Some clones are 6 mm tall (see README): set SENS_H and the deck gets 4 mm shorter.
SENS_L, SENS_W, SENS_H = 13.0, 6.0, 10.0   # MEASURE
SENS_GAP = 5.0                             # MEASURE
SENS_AXIS = 3.0                            # optical axis below the top. MEASURE
SENS_SLOT_D = 7.0                          # slot depth from the top. MEASURE
SENS_COVER = 1.5                           # comb teeth reach this far past the axis
TEETH = 60                                 # x 4 (quadrature) = 240 counts per turn
# Two sensors across the axis; the second a quarter tooth on, so A and B are 90 deg apart.
SENS_ANG = (0.0, 180.0 + 360.0 / TEETH / 4)

# ---------------------------------------------------------------- body
WALL = 1.8
BASE_T = 1.5
PLATE_T = 1.6            # the deck plate: post on top, electronics bay under it (thick enough
                         # for M2 countersunk heads to sit flush underneath)
EDGE_R = 1.0

# electronics stack (bottom up); the main PCB is clamped between the base and the shell's bosses
MAIN_Z = BASE_T + 1.0                 # 1 mm for through-hole leads under it
MAIN_T = 1.6
MAIN_TOP = MAIN_Z + MAIN_T            # 4.1
XIAO = (17.8, 21.0, 1.0)              # Seeed XIAO ESP32-S3
XIAO_H = 3.3                          # tallest part above its board. MEASURE
XIAO_TOP = MAIN_TOP + XIAO[2] + XIAO_H
USBC = (8.9, 3.2, 7.3)
USBC_OVER = 1.3
BATT = (30.0, 20.0, 4.0)              # 402030 LiPo, on the main PCB. MEASURE
JACK = (12.0, 6.0, 5.0)               # PJ-320D style: length, width, height. MEASURE
JACK_BARREL = (5.0, 2.0)

PLATE_Z = XIAO_TOP + 0.4              # underside of the deck plate
PLATE_TOP = PLATE_Z + PLATE_T
assert MAIN_TOP + BATT[2] + 0.4 <= PLATE_Z, "LiPo hits the deck plate"

# comb and sensors
COMB_BOT = PLATE_TOP + 0.5
SENS_TOP = COMB_BOT + SENS_COVER + SENS_AXIS
SENS_LIFT = SENS_TOP - SENS_H - MAIN_TOP   # sensors stand this far off the main PCB
assert SENS_LIFT >= 0, ("sensors too tall for the stack: raise PLATE_Z", SENS_LIFT)
COMB_T = 1.2                          # radial
SLOT_TOP = COMB_BOT + SENS_COVER + SENS_AXIS - 2.0   # teeth from the comb's bottom to here

# bearing and post
FLANGE_Z = SENS_TOP + 0.5             # hub flange (carries the outer race) clears the sensors
FLANGE_T = 1.2
BRG_Z = FLANGE_Z + FLANGE_T
BRG_TOP = BRG_Z + BRG[2]
STEP = (BRG_IN_LAND - 0.2, 1.0)       # post shoulder under the inner race: dia, height
POST_TOP = BRG_TOP - 0.1              # the washer clamps the inner race, not the post
POST_BORE = 27.0                      # screen cable comes down it (the connector is 26.2 across)
POST_LOW_D = 38.0                     # post below the bearing seat
POST_FLAT = 16.6                      # cut back to this at each sensor, below the step
POST_CB = (4.2, 4.0)                  # screen screws from below: counterbore dia, solid left at the top
POST_FIX_R = 17.2                     # 2 M2 thread-formers up through the plate into the post
POST_FIX_ANG = (90.0, 270.0)
WASHER = (BRG_IN_LAND - 0.2, POST_BORE, 1.0)   # OD, ID, thickness
SCR_Z = BRG_TOP + WASHER[2]           # standoff feet
GLASS_TOP = SCR_Z + SCR_STACK
SCR_PCB_Z = GLASS_TOP - GLASS_TO_PCB - SCR_PCB_T

# comb radius: just outside the sensor's inner arm clearing the post
_inner_arm = SENS_L / 2 - SENS_GAP / 2
COMB_R = POST_FLAT + 0.5 + _inner_arm + SENS_GAP / 2     # comb centre
SENS_R = COMB_R                                          # sensor slot centred on the comb
SENS_IN_R, SENS_OUT_R = SENS_R - SENS_L / 2, SENS_R + SENS_L / 2

# ---------------------------------------------------------------- jog (hub + face)
HUB_IN = BRG[1] + BRG_FIT
HUB_OUT = HUB_IN + 4.0                # 2 mm wall round the outer race
FLANGE_IN = 2 * (COMB_R - COMB_T / 2)
assert FLANGE_IN / 2 > STEP[0] / 2 + 0.6, "hub flange rubs the post's shoulder"
assert FLANGE_IN / 2 < BRG_OUT_LAND / 2 + 0.6, "hub flange doesn't reach under the outer race"
FACE_GAP = 0.5                        # face above the glass
FACE_TOP = GLASS_TOP + FACE_GAP
FACE_T = 1.6                          # top skin
FACE_ID = LENS_D + 1.0                # 0.5 mm round the glass
FACE_INNER_WALL = 1.4
FACE_INNER_LOW = SCR_PCB_Z + SCR_PCB_T + 0.4   # inner wall stops above the screen board
PRESS_IN = BRG_OUT_LAND + 0.4         # face's press tube on the outer race (to the hub's bore)
SOCKET = (HUB_OUT + 0.2, 1.6, 4.5)    # glued over the hub: ID, wall, overlap down the hub
RIM_T = 1.6
RIM_GAP = 0.4                         # rim's bottom edge above the shell's top
KNURL = (90, 0.45)                    # flutes round the rim, depth
MARK = (1.2, 5.0, 0.6)                # line on the face: width, length, depth

# ---------------------------------------------------------------- outside
R_OUT = max(SENS_OUT_R + 0.6 + WALL, SOCKET[0] / 2 + SOCKET[1] + 0.4 + RIM_T)
R_OUT = math.ceil(R_OUT * 2) / 2      # to 0.5 mm
DIA = 2 * R_OUT
R_IN = R_OUT - WALL
H = FACE_TOP
assert SENS_OUT_R + 0.5 < R_IN, "sensor hits the wall"
assert R_OUT - RIM_T > SENS_OUT_R + 0.5, "sensor hits the rim"

# ---------------------------------------------------------------- bay layout (plan)
XIAO_C = (0.0, math.sqrt((R_IN - 0.3) ** 2 - (XIAO[0] / 2) ** 2) - XIAO[1] / 2 - 0.05)   # USB-C out the back
BATT_C = (0.0, -(R_IN - 0.6) + BATT[1] / 2 + 4.3)
JACK_Y = 10.0                                                     # out the left side
JACK_X0 = -math.sqrt((R_IN - 0.4) ** 2 - (JACK_Y + JACK[1] / 2) ** 2)   # outer end of the body
DAC_C = (-12.5, 2.0)
# shell bosses: base screws up through the base and the main PCB (clamped) into them
BOSS_ANG = (45.0, 135.0, 225.0, 315.0)
BOSS_R = R_IN - 2.0
BOSS_D = 5.0
BOSS_PILOT = (1.6, 3.5)
for _a in BOSS_ANG:
    _bx, _by = ang(_a, BOSS_R)
    assert abs(_by) > 3 + BOSS_D / 2 or abs(_bx) < SENS_IN_R - BOSS_D / 2, "boss hits a sensor"

def bbox_ok(c, size, r=R_IN - 0.3):
    (cx, cy), (w, d) = c, size
    return all(math.hypot(cx + sx * w / 2, cy + sy * d / 2) < r for sx in (-1, 1) for sy in (-1, 1))
assert bbox_ok(XIAO_C, XIAO[:2]), "XIAO pokes through the wall"
assert bbox_ok(BATT_C, BATT[:2]), "LiPo pokes through the wall"
assert BATT_C[1] + BATT[1] / 2 < -SENS_W / 2 - 0.5, "LiPo hits a sensor"
assert XIAO_C[1] - XIAO[1] / 2 > SENS_W / 2 + 0.5, "XIAO hits a sensor"
assert JACK_Y - JACK[1] / 2 > SENS_W / 2 + 0.5, "jack hits a sensor"

if __name__ == "__main__":
    print(f"puck deck {DIA:.1f} dia x {H:.1f} mm; jog ring {FACE_ID:.1f}-{DIA:.1f}")
    print(f"  bay: main PCB {MAIN_Z:.1f}-{MAIN_TOP:.1f}, XIAO top {XIAO_TOP:.1f}, plate {PLATE_Z:.1f}-{PLATE_TOP:.1f}")
    print(f"  comb r {COMB_R:.1f}, {COMB_BOT:.1f}-{FLANGE_Z:.1f}, teeth to {SLOT_TOP:.1f}; sensors r {SENS_IN_R:.1f}-{SENS_OUT_R:.1f}, top {SENS_TOP:.1f} (lift {SENS_LIFT:.1f})")
    print(f"  bearing {BRG_Z:.1f}-{BRG_TOP:.1f}, screen feet {SCR_Z:.1f}, board {SCR_PCB_Z:.1f}, glass top {GLASS_TOP:.1f}, face top {FACE_TOP:.1f}")
