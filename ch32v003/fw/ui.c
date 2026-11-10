// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.h"
#include "app.h"
#include "hal.h"

#define OLED     0x3C
#define FRAME_MS 50
#ifndef WAVE_DIV
#define WAVE_DIV 1                        // the waveform every Nth frame (2: 10 fps)
#endif
#define WAVE_X   32                       // playhead column: 0.37 s behind it, 1.1 s ahead

static uint8_t  s_buf[UI_BUF] __attribute__((aligned(4)));   // control byte + one page
static uint8_t  s_page = 0xFF;            // page being sent; 0xFF: between frames
static uint8_t  s_addr;                   // 1: its address went, the data is next
static uint8_t  s_frame;
static uint32_t s_next;

static const uint8_t kInit[] = {
    0x00,                                 // commands
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,   // multiplex 64
    0x20, 0x00,                           // horizontal addressing: pages follow each other
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,   // alternate COM pins
};
// Each page is addressed on its own (so a frame can leave some pages as they are).
#define ADDR(p) { 0x00, 0x21, 0, 127, 0x22, p, p }
static const uint8_t kAddr[UI_PAGES][7] = { ADDR(0), ADDR(1), ADDR(2), ADDR(3), ADDR(4), ADDR(5), ADDR(6), ADDR(7) };

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

// page 3: state, keylock, tempo range, cue point
static void status(void)
{
    char s[10], *p = s;
    text(0, g_app.state == ST_PLAY ? "PLAY" : g_app.state == ST_CUE ? "CUE" : "PAUSE", 0);
    if (g_app.keylock) {                                      // inverted tag
        for (uint8_t i = 32; i < 53; i++) s_buf[1 + i] = 0xFF;
        text(34, "KEY", 0xFF);
    }
    *p++ = '+'; *p++ = '-'; p = dec(p, g_app.range); *p = 0;
    text(58, s, 0);
    p = s; *p++ = 'C'; *p++ = ' '; p = mmss(p, g_app.cue); *p = 0;
    text((uint8_t)(128 - 6 * (p - s)), s, 0);
}

// pages 4-6: the waveform around the playhead, 24 px, mirrored about its middle. One peak a
// column (11.6 ms); read from the card straight into the page buffer and turned into pixels
// in place, so it needs no RAM of its own. Beat ticks top and bottom (downbeats longer),
// the playhead inverted, the cue a notch at the top, the past dimmed.
static void wave(uint8_t band)
{
#ifdef UI_NO_WAVE
    (void)band; return;                                       // for measuring without it
#endif
    if (!g_app.peaksOff) return;
    uint8_t* px = s_buf + 1;
    const uint32_t len = (g_app.frames + TRK_PEAK - 1) / TRK_PEAK;
    const int32_t c0 = (int32_t)(pos() / TRK_PEAK) - WAVE_X;          // the first column's peak
    uint8_t skip = 0, n = 128;
    if (c0 < 0) skip = (uint8_t)(-c0 > 128 ? 128 : -c0);
    if (c0 + 128 > (int32_t)len) n = (uint8_t)((int32_t)len - c0 < 0 ? 0 : (int32_t)len - c0);
    if (n > skip) trk_read(g_app.peaksOff + (uint32_t)(c0 + skip), px + skip, (uint16_t)(n - skip));
    const uint8_t top = (uint8_t)(band << 3);
    for (uint8_t x = 0; x < 128; x++) {
        uint8_t b = 0;
        if (x >= skip && x < n) {
            const uint8_t h = (uint8_t)((px[x] * 11 + 127) >> 8);  // 0..11 each way of row 11:
            const uint8_t lo = 11 - h, hi = 12 + h;                 // at least 1 px, so it joins up
            for (uint8_t r = 0; r < 8; r++) if (top + r >= lo && top + r < hi) b |= (uint8_t)(1 << r);
            if (x < WAVE_X) b &= (x & 1) ? 0xAA : 0x55;
        }
        px[x] = b;
    }
    if (g_app.bpmX100) {                                      // beat ticks
        const uint32_t beat = 132300000UL / g_app.bpmX100;     // frames per beat
        const int32_t f0 = c0 * TRK_PEAK;                       // the window's first frame
        uint32_t k = 0, f = g_app.firstBeat;
        if (f0 > (int32_t)f) { k = ((uint32_t)f0 - f + beat - 1) / beat; f += k * beat; }
        for (; (int32_t)f < f0 + 128 * TRK_PEAK; f += beat, k++) {
            const int32_t x = ((int32_t)f - f0) / TRK_PEAK;
            if (x < 0 || x > 127) continue;
            const uint8_t m = (k & 3) ? 0x03 : 0x0F;
            if (band == 0) px[x] |= m;
            if (band == 2) px[x] |= (uint8_t)(m << ((k & 3) ? 6 : 4));
        }
    }
    const int32_t cx = (int32_t)(g_app.cue / TRK_PEAK) - c0;
    if (band == 0 && cx >= 0 && cx < 128) {
        px[cx] ^= 0x07;
        if (cx > 0) px[cx - 1] ^= 0x03;
        if (cx < 127) px[cx + 1] ^= 0x03;
    }
    px[WAVE_X] = (uint8_t)~px[WAVE_X];
}

// page 7: the whole track, 8 px
static void overview(void)
{
    if (g_app.frames < 128) return;
    const uint32_t per = g_app.frames >> 7;
    const uint32_t head = pos() / per, cue = g_app.cue / per;
    for (uint8_t x = 0; x < 128; x++) {
        const uint8_t h = (uint8_t)((((g_app.ov[x >> 1] >> ((x & 1) << 2)) & 15) + 1) >> 1);   // 0..8
        uint8_t b = h ? (uint8_t)(0xFF << (8 - h)) : 0;
        if (x < head) b &= (x & 1) ? 0xAA : 0x55;
        if (x == head) b = (uint8_t)~b;
        if (x == cue) b ^= 0x07;
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
    case 7: overview(); break;
    default: wave(page - 4); break;
    }
}

uint8_t* ui_borrow(void)
{
    s_page = 0xFF; s_addr = 0;
    return s_buf;
}

static int skipped(uint8_t page)
{
    return WAVE_DIV > 1 && page >= 4 && page <= 6 && !g_app.browsing && s_frame % WAVE_DIV;
}

void ui_step(void)
{
    if (hal_i2c_busy()) return;
    if (s_page == 0xFF) {
        if ((int32_t)(hal_ms() - s_next) < 0) return;
        s_next = hal_ms() + FRAME_MS;
        s_frame++;
        s_page = 0;
    }
    if (!s_addr) {
        while (s_page < UI_PAGES && skipped(s_page)) s_page++;
        if (s_page == UI_PAGES) { s_page = 0xFF; return; }
        hal_i2c_send(OLED, kAddr[s_page], sizeof kAddr[0]);
        s_addr = 1;
        return;
    }
    render(s_page);
    s_buf[0] = 0x40;                      // data
    hal_i2c_send(OLED, s_buf, sizeof s_buf);
    s_addr = 0;
    if (++s_page == UI_PAGES) s_page = 0xFF;
}
