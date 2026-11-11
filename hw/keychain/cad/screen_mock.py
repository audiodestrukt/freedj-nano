# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""screen_mock.py: a 96 x 64 picture of a play screen for the colour OLED, render only
(drawn at 1 px = 1 OLED pixel, then scaled up with nearest-neighbour so it looks like one).
python3 screen_mock.py -> build/screen.png
"""
import math, random
from PIL import Image, ImageDraw, ImageFont
import params as M

W, H = 96, 64
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

def main():
    random.seed(3)
    im = Image.new("RGB", (W, H), (0, 0, 0)); d = ImageDraw.Draw(im)
    d.text((1, 0), "NIGHT BUS", font=ImageFont.truetype(FONT, 8), fill=(220, 224, 230))
    d.rectangle((76, 1, 94, 8), fill=(47, 191, 113)); d.text((78, 0), "PLAY", font=ImageFont.truetype(FONT, 6), fill=(0, 0, 0))
    d.text((1, 9), "1:42", font=ImageFont.truetype(FONT, 15), fill=(240, 240, 240))
    d.text((48, 12), "+1.2%", font=ImageFont.truetype(FONT, 9), fill=(242, 157, 56))
    d.text((60, 21), "126.0", font=ImageFont.truetype(FONT, 7), fill=(140, 146, 155))
    # overview strip with playhead and cue triangle
    d.rectangle((1, 29, 94, 33), fill=(26, 30, 36))
    for x in range(2, 94):
        e = 0.5 + 0.4 * math.sin(x * 0.31) * math.sin(x * 0.07)
        d.line((x, 33, x, 33 - int(4 * abs(e))), fill=(61, 126, 255) if x < 40 else (36, 58, 92))
    d.line((40, 28, 40, 34), fill=(255, 255, 255))
    d.polygon(((20, 34), (18, 37), (22, 37)), fill=(242, 157, 56))
    # scrolling waveform, coloured by band like the TFT build
    cy = 50
    for x in range(W):
        t = x * 0.33
        beat = (t * 0.5) % 1.0
        e = 0.35 + 0.55 * math.exp(-beat * 6) + random.uniform(-0.08, 0.08)
        h = max(1, int(12 * min(1.0, e)))
        col = (255, 90, 60) if beat < 0.15 else (80, 170, 255) if beat < 0.6 else (90, 230, 160)
        d.line((x, cy - h, x, cy + h), fill=col)
    d.line((W // 2, 38, W // 2, 63), fill=(255, 255, 255))
    im = im.resize((W * 6, H * 6), Image.NEAREST)
    im.save(M.out("screen.png"))
    print("wrote", M.out("screen.png"))

if __name__ == "__main__":
    main()
