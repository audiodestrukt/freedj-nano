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
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,   // multiplex 64
    0x20, 0x00,                           // horizontal addressing: pages follow each other
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,   // alternate COM pins
};
static const uint8_t kHome[] = { 0x00, 0x21, 0, 127, 0x22, 0, UI_PAGES - 1 };   // all columns, all pages

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

// 5x7 at twice the size over two pages: `half` 0 the top page, 1 the bottom.
static uint8_t big(uint8_t x, const char* s, uint8_t half)
{
    uint8_t* px = s_buf + 1;
    for (; *s && x <= 116; s++) {
        const uint8_t* g = ui_font[*s - 32];
        for (uint8_t k = 0; k < 5; k++) {
            const uint8_t v = (uint8_t)(g[k] >> (half << 2));
            uint8_t b = 0;
            for (uint8_t i = 0; i < 4; i++) if (v >> i & 1) b |= (uint8_t)(3 << (2 * i));
            px[x++] = b; px[x++] = b;
        }
        x += 2;
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

static char* mmss(char* p, uint32_t frames)
{
    const uint32_t t = frames / 22050;
    p = dec(p, t / 60); *p++ = ':'; return num(p, t % 60, 2);
}

static uint32_t pos(void) { const int32_t p = lite_pos(&g_deck); return p < 0 ? 0 : (uint32_t)p; }

// pages 1-2: time left, large; BPM at this tempo (page 1) and tempo (page 2) on the right
static void timeAndTempo(uint8_t half)
{
    char s[12], *p = s;
    const uint32_t at = pos();
    *p++ = '-'; p = mmss(p, at < g_app.frames ? g_app.frames - at : 0); *p = 0;
    big(0, s, half);
    p = s;
    if (!half) {
        if (!g_app.bpmX100) return;
        const uint32_t b = g_app.bpmX100 * (uint32_t)(1000 + g_app.tempo) / 10000;   // BPM x 10
        p = dec(p, b / 10); *p++ = '.'; *p++ = (char)('0' + b % 10);
    } else {
        const int16_t t = g_app.tempo;
        const uint16_t a = (uint16_t)(t < 0 ? -t : t);
        *p++ = t < 0 ? '-' : '+'; p = dec(p, a / 10); *p++ = '.'; *p++ = (char)('0' + a % 10); *p++ = '%';
    }
    *p = 0;
    text((uint8_t)(128 - 6 * (p - s)), s, 0);
}

// page 3: state, keylock, tempo range
static void status(void)
{
    char s[6], *p = s;
    text(0, g_app.state == ST_PLAY ? "PLAY" : g_app.state == ST_CUE ? "CUE" : "PAUSE", 0);
    if (g_app.keylock) {                                      // inverted tag
        for (uint8_t i = 46; i < 69; i++) s_buf[1 + i] = 0xFF;
        text(48, "KEY", 0xFF);
    }
    *p++ = '+'; *p++ = '-'; p = dec(p, g_app.range); *p++ = '%'; *p = 0;
    text((uint8_t)(128 - 6 * (p - s)), s, 0);
}

// page 4: the bar as four beats, the current one filled (needs the track's beat grid)
static void beats(void)
{
    if (!g_app.bpmX100) return;
    const uint32_t at = pos();
    if (at < g_app.firstBeat) return;
    const uint32_t len = 132300000UL / g_app.bpmX100;        // frames per beat: 60 x 22050 x 100 / bpm x 100
    const uint32_t n = (at - g_app.firstBeat) / len;
    for (uint8_t b = 0; b < 4; b++) {
        const uint8_t x0 = (uint8_t)(b * 32), on = (n & 3) == b;
        for (uint8_t x = x0 + 1; x < x0 + 31; x++)
            s_buf[1 + x] = on ? 0x7E : (x == x0 + 1 || x == x0 + 30) ? 0x7E : 0x42;
    }
}

// page 5: elapsed, and the cue point
static void times(void)
{
    char s[16], *p;
    *mmss(s, pos()) = 0;
    text(0, s, 0);
    p = s; *p++ = 'C'; *p++ = 'U'; *p++ = 'E'; *p++ = ' '; p = mmss(p, g_app.cue); *p = 0;
    text((uint8_t)(128 - 6 * (p - s)), s, 0);
}

static void overview(uint8_t lower)
{
    if (g_app.frames < 128) return;
    const uint32_t per = g_app.frames >> 7;
    const uint32_t head = pos() / per, cue = g_app.cue / per;
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
    if (g_app.browsing) {                                     // a header, then 7 names
        const uint8_t rows = UI_PAGES - 1;
        if (page == 0) {
            char s[20], *p = s;
            *p++ = 'T'; *p++ = 'R'; *p++ = 'A'; *p++ = 'C'; *p++ = 'K'; *p++ = 'S'; *p++ = ' ';
            p = dec(p, g_app.sel + 1u); *p++ = '/'; p = dec(p, g_app.count); *p = 0;
            text(0, s, 0);
            for (uint8_t x = 0; x < 128; x++) s_buf[1 + x] |= 0x80;   // underline
            return;
        }
        uint16_t top = g_app.sel > 2 ? g_app.sel - 2 : 0;
        if (g_app.count <= rows) top = 0;
        else if (top > g_app.count - rows) top = g_app.count - rows;
        const uint16_t i = top + page - 1;
        if (i < g_app.count) row(i, i == g_app.sel ? 0xFF : 0);
        return;
    }
    if (g_app.track == APP_NONE) return;
    switch (page) {
    case 0: {
        char s[4]; *num(s, g_app.track + 1, 2) = 0;
        text(text(0, s, 0) + 6, g_app.name, 0);
        break;
    }
    case 1: case 2: timeAndTempo(page - 1); break;
    case 3: status(); break;
    case 4: beats(); break;
    case 5: times(); break;
    default: overview(page == 7); break;
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
    if (++s_page == UI_PAGES) s_page = 0xFF;
}
