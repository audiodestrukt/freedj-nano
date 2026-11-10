// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.h"
#include "app.h"
#include "hal.h"

#define OLED     0x3C
#define FRAME_MS 50

static uint8_t  s_buf[129];               // control byte + one page
static uint8_t  s_page = 0xFF;            // page being sent; 0xFF: between frames
static uint32_t s_next;

static const uint8_t kInit[] = {
    0x00,                                 // commands
    0xAE, 0xD5, 0x80, 0xA8, 0x1F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
    0x20, 0x00,                           // horizontal addressing: pages follow each other
    0xA1, 0xC8, 0xDA, 0x02, 0x81, 0x8F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
};
static const uint8_t kHome[] = { 0x00, 0x21, 0, 127, 0x22, 0, 3 };   // columns 0-127, pages 0-3

void ui_begin(void) { hal_i2c_send(OLED, kInit, sizeof kInit); }

// ---- drawing into the page

static uint8_t text(uint8_t x, const char* s, uint8_t inv)
{
    uint8_t* px = s_buf + 1;
    for (; *s && x <= 122; s++) {
        const char c = *s < 32 || *s > 126 ? '?' : *s;
        const uint8_t* g = ui_font[c - 32];
        for (uint8_t k = 0; k < 5; k++) px[x++] = g[k] ^ inv;
        px[x++] = inv;
    }
    return x;
}

static char* num(char* p, uint32_t v, uint8_t digits)    // zero-padded
{
    p += digits;
    char* e = p;
    while (digits--) { *--p = (char)('0' + v % 10); v /= 10; }
    return e;
}

static char* dec(char* p, uint32_t v)                     // no padding
{
    uint8_t d = 1;
    for (uint32_t t = v; t >= 10; t /= 10) d++;
    return num(p, v, d);
}

static void row(uint16_t i, uint8_t inv)
{
    char s[TRK_NAME + 4];
    char* p = num(s, i + 1, 2);
    *p++ = ' ';
    trk_name(i, p);
    if (inv) for (uint8_t x = 0; x < 128; x++) s_buf[1 + x] = 0xFF;
    text(0, s, inv);
}

static void status(void)
{
    char s[24], *p = s;
    const uint32_t pos = (uint32_t)lite_pos(&g_deck);
    const uint32_t left = pos < g_app.frames ? (g_app.frames - pos) / 22050 : 0;
    *p++ = '-'; p = dec(p, left / 60); *p++ = ':'; p = num(p, left % 60, 2); *p++ = ' ';
    if (g_app.bpmX100) {
        const uint32_t b = g_app.bpmX100 * (uint32_t)(1000 + g_app.tempo) / 10000;   // BPM x 10
        p = dec(p, b / 10); *p++ = '.'; *p++ = (char)('0' + b % 10); *p++ = ' ';
    }
    const int16_t t = g_app.tempo;
    *p++ = t < 0 ? '-' : '+';
    const uint16_t a = (uint16_t)(t < 0 ? -t : t);
    p = dec(p, a / 10); *p++ = '.'; *p++ = (char)('0' + a % 10);
    *p = 0;
    text(0, s, 0);
    if (g_app.keylock) text(104, "K", 0);
    text(116, g_app.state == ST_PLAY ? ">" : g_app.state == ST_CUE ? "C" : "|", 0);
}

static void overview(uint8_t lower)
{
    if (g_app.frames < 128) return;
    const uint32_t per = g_app.frames >> 7;
    const uint32_t head = (uint32_t)lite_pos(&g_deck) / per, cue = g_app.cue / per;
    for (uint8_t x = 0; x < 128; x++) {
        const uint8_t h = (g_app.ov[x >> 1] >> ((x & 1) << 2)) & 15;
        uint8_t b = lower ? (h >= 7 ? 0xFF : (uint8_t)(0xFF << (7 - h)))
                          : (h >= 8 ? (uint8_t)(0xFF << (15 - h)) : 0);
        if (x < head) b &= (x & 1) ? 0xAA : 0x55;           // played: dimmed
        if (x == head) b = (uint8_t)~b;                      // playhead: inverted
        if (!lower && x == cue) b ^= 0x0F;                   // cue: a notch (or a tick) at the top
        if (!lower && (x + 1u == cue || x == cue + 1)) b ^= 0x03;
        s_buf[1 + x] = b;
    }
}

static const char* const kErr[] = { "", "no SD card", "no FAT32", "no TRACKS.BIN", "TRACKS.BIN fragmented", "bad TRACKS.BIN" };

static void render(uint8_t page)
{
    for (uint8_t x = 0; x < 128; x++) s_buf[1 + x] = 0;
    if (g_app.err || !g_app.count) {
        if (page == 0) text(0, "freedj nano", 0);
        if (page == 1) text(0, g_app.err ? kErr[g_app.err] : "no tracks", 0);
        return;
    }
    if (g_app.browsing) {
        uint16_t top = g_app.sel ? g_app.sel - 1 : 0;
        if (g_app.count > 4 && top > g_app.count - 4) top = g_app.count - 4;
        if (g_app.count <= 4) top = 0;
        if (top + page < g_app.count) row(top + page, top + page == g_app.sel ? 0xFF : 0);
        return;
    }
    if (g_app.track == APP_NONE) return;
    if (page == 0) {
        char s[4]; *num(s, g_app.track + 1, 2) = 0;
        text(text(0, s, 0) + 6, g_app.name, 0);
    } else if (page == 1) {
        status();
    } else {
        overview(page == 3);
    }
}

void ui_step(void)
{
    if (hal_i2c_busy()) return;
    if (s_page == 0xFF) {
        if ((int32_t)(hal_ms() - s_next) < 0) return;
        s_next = hal_ms() + FRAME_MS;
        hal_i2c_send(OLED, kHome, sizeof kHome);
        s_page = 0;
        return;
    }
    render(s_page);
    s_buf[0] = 0x40;                      // data
    hal_i2c_send(OLED, s_buf, sizeof s_buf);
    if (++s_page == 4) s_page = 0xFF;
}
