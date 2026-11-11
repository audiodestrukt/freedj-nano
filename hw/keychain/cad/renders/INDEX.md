# keychain deck render log

- 0001-first-layout-hero.jpg: 2026-11-10: first layout, 54 x 76 x 16.9 mm (+7 mm keyring tab). 0.95" colour OLED over a 32 mm jog, 8 mm CUE / PLAY in a row at the front, 45 mm fader down the right side
- 0002-first-layout-top.jpg: 2026-11-10: same, from above
- 0003-back-usb-c-keyring.jpg: 2026-11-10: from the back: the XIAO S3's USB-C (stick / charging) with a relief for a stick's overmould, keyring tab flush with the top
- 0004-inside-shell-hidden.jpg: 2026-11-10: shell, platter, caps and OLED hidden: main PCB with a well for the AS5600 breakout, the bearing pair over the chip, XIAO at the back, pot soldered through the PCB, jack behind it, 6 x 6 tact switches at the front
- 0005-section-through-jog.jpg: 2026-11-10: section at the jog axis: platter + printed 6 mm shaft through two MR106ZZ (1 mm ledge between), glued collar under the lower one, magnet flush in the tip 1.0 mm over the AS5600; OLED over the XIAO, LiPo under the main PCB
- 0006-cdj-layout-35mm-hero.jpg: 2026-11-10: CDJ layout: CUE over PLAY in the bottom-left corner, jog moved right to clear CUE by 1.2 mm; 35 mm fader (50 mm frame, M2s 46 apart) centred on the jog. 55 x 76 x 16.9 mm
- 0007-cdj-layout-35mm-top.jpg: 2026-11-10: same, from above
- 0008-cdj-layout-35mm-inside.jpg: 2026-11-10: same, shell hidden: tact switches stacked at the front-left, shorter pot, jack now 4 mm clear of the back corner
- 0009-fader-at-front-edge-top.jpg: 2026-11-10: fader moved forward until its frame is 0.3 mm off the front wall (slot 7.3-46.9 from the front); inside corners tightened to 1 mm so the frame clears the front-right corner
- 0010-shell-underside-base-bosses.jpg: 2026-11-10: shell from below (as printed): the base bosses' M2 pilots now open toward the base (they opened upward before, so the base screws had nothing to bite); the front-right boss moved left of the fader frame
- 0011-platter-in-line-with-play-hero.jpg: 2026-11-10: jog moved down so the platter's front edge is 0.5 mm behind PLAY's (as on a CDJ); it moves right to keep clear of CUE, the screen comes forward with it. 60 x 63.6 x 16.9 mm (depth/width 1.06, near freedj-esp's 1.10-1.16)
- 0012-platter-in-line-with-play-top.jpg: 2026-11-10: same, from above
- 0013-platter-in-line-with-play-inside.jpg: 2026-11-10: same, shell hidden; the DAC moved right of the XIAO (it landed on it when the XIAO came forward)
- 0014-no-keyring-tab-hero.jpg: 2026-11-10: keyring tab left off for now (KEYRING = False in params.py). 60 x 63.6 x 16.9 mm
- 0015-current-hero.jpg .. 0021-current-shell-underside.jpg: 2026-11-10: full set of the current design (60 x 63.6 x 16.9 mm, CDJ layout, platter in line with PLAY, 35 mm fader at the front edge, no keyring tab):
  0015 hero, 0016 top, 0017 back (USB-C), 0018 right side (headphone jack), 0019 inside (shell hidden), 0020 section through the jog, 0021 shell underside (bosses and pilots)
- 0022-tft20-hero.jpg .. 0027-tft20-shell-underside.jpg: 2026-11-10: KC_SCREEN=tft20, the default now: Waveshare-style 2" ST7789 (320 x 240) in landscape across the deck behind the jog, showing the firmware's real play screen (docs/tft_320x240.png). 20 mm fader (35 mm frame) so it ends 1.6 mm before the screen's board; it hangs on 1.3 mm pads (the deck is taller, so its pins still reach the main PCB) and the knob is 1.3 mm lower. Jack moved to the left wall between CUE and the screen, brand runs up the strip left of the window. 62.4 x 76 x 18.2 mm. The 0.95" OLED version (KC_SCREEN=oled095) is unchanged at 60 x 63.6 x 16.9.
  0022 hero, 0023 top, 0024 left side (jack), 0025 inside, 0026 section through the jog, 0027 shell underside
- 0028-centred-tft20-hero.jpg .. 0030-centred-oled095-top.jpg: 2026-11-11: jog and screen window both on the deck's centreline. The 2" active area sits 1.7 mm off-centre on its board, so the board shifts left and the deck grows to 65.8 mm wide (65.8 x 76 x 18.2); CUE / PLAY stay in the corner and the fader against the right wall, now 6 mm off the platter. The OLED deck is 60.4 x 63.6 x 16.9.
  0028 tft20 hero, 0029 tft20 top, 0030 oled095 top
