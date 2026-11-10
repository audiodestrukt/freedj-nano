// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "oled_emu.h"

void oled_emu_reset(oled_emu* o)
{
    for (int p = 0; p < 8; p++) for (int x = 0; x < 128; x++) o->ram[p][x] = 0;
    o->on = 0; o->invert = 0; o->segRemap = 0; o->comScan = 0;
    o->mode = 2;                                          // page addressing after reset
    o->col = 0; o->page = 0; o->c0 = 0; o->c1 = 127; o->p0 = 0; o->p1 = 7; o->mux = 63;
    o->ncmd = 0; o->need = 0;
}

static uint8_t args(uint8_t c)
{
    switch (c) {
    case 0x20: case 0x81: case 0x8D: case 0xA8: case 0xD3: case 0xD5: case 0xD9: case 0xDA: case 0xDB: return 1;
    case 0x21: case 0x22: case 0xA3: return 2;
    case 0x29: case 0x2A: return 5;
    case 0x26: case 0x27: return 6;
    default: return 0;
    }
}

static void command(oled_emu* o)
{
    const uint8_t c = o->cmd[0];
    switch (c) {
    case 0xAE: case 0xAF: o->on = c & 1; break;
    case 0xA6: case 0xA7: o->invert = c & 1; break;
    case 0xA0: case 0xA1: o->segRemap = c & 1; break;
    case 0xC0: case 0xC8: o->comScan = c == 0xC8; break;
    case 0x20: o->mode = o->cmd[1] & 3; break;
    case 0x21: o->c0 = o->cmd[1] & 127; o->c1 = o->cmd[2] & 127; o->col = o->c0; break;
    case 0x22: o->p0 = o->cmd[1] & 7; o->p1 = o->cmd[2] & 7; o->page = o->p0; break;
    case 0xA8: o->mux = o->cmd[1] & 63; break;
    default:
        if (c >= 0xB0 && c <= 0xB7) o->page = c & 7;
        else if (c <= 0x0F) o->col = (o->col & 0xF0) | c;
        else if (c >= 0x10 && c <= 0x1F) o->col = (uint8_t)(((c & 7) << 4) | (o->col & 15));
        break;
    }
}

static void data(oled_emu* o, uint8_t b)
{
    o->ram[o->page & 7][o->col & 127] = b;
    o->writes++;
    if (o->mode == 0) {                                   // horizontal
        if (o->col++ >= o->c1) { o->col = o->c0; if (o->page++ >= o->p1) o->page = o->p0; }
    } else if (o->mode == 1) {                            // vertical
        if (o->page++ >= o->p1) { o->page = o->p0; if (o->col++ >= o->c1) o->col = o->c0; }
    } else if (o->col < 127) {
        o->col++;
    }
}

void oled_emu_write(oled_emu* o, uint8_t addr, const uint8_t* p, uint16_t n)
{
    o->bytes += n;
    if (addr != 0x3C) { o->badAddr++; return; }
    uint16_t i = 0;
    while (i < n) {
        const uint8_t ctl = p[i++];
        const int isData = ctl & 0x40, one = ctl & 0x80;  // Co: one byte, then another control byte
        do {
            if (i >= n) return;
            const uint8_t b = p[i++];
            if (isData) { data(o, b); continue; }
            if (!o->need) { o->ncmd = 0; o->need = args(b) + 1; }
            o->cmd[o->ncmd++] = b;
            if (--o->need == 0) command(o);
        } while (!one);
    }
}

int oled_emu_pixel(const oled_emu* o, int x, int y)
{
    if (!o->on) return 0;
    const int c = o->segRemap ? x : 127 - x;              // modules are mounted for A1 / C8
    if (y > o->mux) return 0;                             // rows past the multiplex ratio stay dark
    const int r = o->comScan ? y : o->mux - y;
    return ((o->ram[r >> 3][c] >> (r & 7)) & 1) ^ o->invert;
}
