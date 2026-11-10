// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// SSD1306 128x64 (0.96") on I2C: one 128-byte page rendered and sent per call, by DMA, so a
// frame never holds up the audio, and the RAM is one page whatever the panel's size.
// 20 frames a second (a frame is ~23 ms of the bus at 400 kHz).
//
//   deck:  0    "01 Track name"
//          1-2  "-3:21" large               "128.0" BPM at this tempo, "+3.2%"
//          3    PLAY / PAUSE / CUE, KEY (keylock, inverted), "+-10%" range
//          4    the bar: four beats, the current one filled (from the beat grid)
//          5    elapsed                     "CUE 0:12"
//          6-7  overview: played part dimmed, playhead inverted, cue notch
//   list:  "TRACKS 3/12", then 7 names, the highlighted one inverted
#include <stdint.h>

#define UI_PAGES 8

extern const uint8_t ui_font[95][5];

void ui_begin(void);
void ui_step(void);
