// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// An SSD1306 128x32 from its I2C byte stream: control bytes, the commands an init sequence
// uses (addressing modes, column / page ranges, segment and COM remap, invert, on / off),
// and GDDRAM writes. What the panel shows is oled_emu_pixel().
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  ram[4][128];
    uint8_t  on, invert, segRemap, comScan, mode, col, page, c0, c1, p0, p1;
    uint8_t  cmd[8], ncmd, need;
    uint32_t bytes, writes, badAddr;
} oled_emu;

void oled_emu_reset(oled_emu* o);
void oled_emu_write(oled_emu* o, uint8_t addr, const uint8_t* p, uint16_t n);
int  oled_emu_pixel(const oled_emu* o, int x, int y);    // 0 / 1 as lit on the glass

#ifdef __cplusplus
}
#endif
