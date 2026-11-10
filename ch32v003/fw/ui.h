// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// SSD1306 128x32 on I2C: one 128-byte page rendered and sent per call, by DMA, so a frame
// never holds up the audio. 20 frames a second.
//
//   deck:  "01 Track name"
//          "-3:21 128.0 +3.2 K >"   remaining, BPM at this tempo, tempo %, keylock, state
//          16 px overview: played part dimmed, playhead, cue mark
//   list:  four names, the highlighted one inverted
#include <stdint.h>

extern const uint8_t ui_font[95][5];

void ui_begin(void);
void ui_step(void);
