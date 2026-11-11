# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""screen_mock.py: a mock-up of a 240 x 240 round play screen -> build/screen.png
No round layout exists in the firmware yet. Drawn at 1 px = 1 LCD pixel (scaled up 4x with
nearest-neighbour for the render): track progress round the edge, time and pitch in the middle,
a scrolling waveform across it, and CUE / PLAY as touch buttons along the bottom.
"""
import math, random
from PIL import Image, ImageDraw, ImageFont
import params as M

N = 240
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
F = lambda s, b=True: ImageFont.truetype(BOLD if b else REG, s)

def main():
    random.seed(5)
    im = Image.new("RGB", (N, N), (0, 0, 0)); d = ImageDraw.Draw(im)
    c = N / 2
    # progress ring: played part bright, cue triangle, beat ticks
    d.arc((4, 4, N - 5, N - 5), 0, 360, fill=(30, 36, 44), width=7)
    d.arc((4, 4, N - 5, N - 5), -90, -90 + 360 * 0.38, fill=(61, 126, 255), width=7)
    a = math.radians(-90 + 360 * 0.12)
    tx, ty = c + (c - 15) * math.cos(a), c + (c - 15) * math.sin(a)
    d.polygon([(tx + 5 * math.cos(a), ty + 5 * math.sin(a)), (tx - 4 * math.sin(a), ty + 4 * math.cos(a)),
               (tx + 4 * math.sin(a), ty - 4 * math.cos(a))], fill=(242, 157, 56))
    ctr = lambda y, s, f, col: d.text((c - d.textlength(s, font=f) / 2, y), s, font=f, fill=col)
    ctr(26, "NIGHT BUS", F(15), (225, 228, 235))
    ctr(45, "deck A  8A", F(11, False), (140, 146, 155))
    ctr(60, "-2:18.4", F(30), (245, 245, 245))
    d.text((48, 96), "+1.2%", font=F(14), fill=(242, 157, 56))
    d.text((132, 96), "126.0", font=F(14), fill=(200, 205, 212))
    # scrolling waveform across the middle, band-coloured like the TFT build
    cy = 140
    for x in range(14, N - 14):
        half = math.sqrt(max(0, (c - 14) ** 2 - (x - c) ** 2))
        if half < 30: continue
        t = x * 0.21
        beat = (t * 0.5) % 1.0
        e = 0.3 + 0.6 * math.exp(-beat * 6) + random.uniform(-0.07, 0.07)
        h = max(1, int(17 * min(1.0, e)))
        col = (255, 90, 60) if beat < 0.15 else (80, 170, 255) if beat < 0.6 else (90, 230, 160)
        if x < c: col = tuple(int(v * 0.55) for v in col)
        d.line((x, cy - h, x, cy + h), fill=col)
    d.line((c, cy - 22, c, cy + 22), fill=(255, 255, 255), width=2)
    # touch buttons
    for x0, label, col, fg in ((62, "CUE", (60, 70, 90), (242, 157, 56)), (124, "PLAY", (40, 120, 75), (230, 255, 235))):
        d.rounded_rectangle((x0, 172, x0 + 54, 198), 7, fill=col)
        d.text((x0 + 27 - d.textlength(label, font=F(13)) / 2, 177), label, font=F(13), fill=fg)
    # round mask
    mask = Image.new("L", (N, N), 0); ImageDraw.Draw(mask).ellipse((0, 0, N - 1, N - 1), fill=255)
    im = Image.composite(im, Image.new("RGB", (N, N)), mask)
    im.resize((N * 4, N * 4), Image.NEAREST).save(M.out("screen.png"))
    print("wrote", M.out("screen.png"))

if __name__ == "__main__":
    main()
