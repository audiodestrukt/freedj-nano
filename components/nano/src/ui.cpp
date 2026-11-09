// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nano/ui.h"

#include <stdio.h>
#include <string.h>

using namespace gfx;

namespace ui {

namespace {

constexpr uint16_t hex(uint32_t c) { return rgb((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c); }
// Mono and shared
constexpr uint16_t kBg = kBlack, kText = kWhite;
constexpr uint16_t kDim = rgb(90, 90, 90), kBar = rgb(40, 40, 48);
constexpr uint16_t kWave = rgb(0, 170, 255), kPlayed = rgb(0, 80, 140);
constexpr uint16_t kHead = rgb(255, 40, 40), kCueC = rgb(255, 150, 0);
constexpr int kPeakMs = 10;   // pack peaks are 10 ms buckets (tools/pack_tracks.py)
// Color layouts: the CDJ-3000 look, in freedj-esp's palette (p4/deck/main/ui.cpp)
constexpr uint16_t cBg = hex(0x0B0D10), cPanel = hex(0x1A1E24), cText = hex(0xE8EAED), cDim = hex(0x8A9099);
constexpr uint16_t cGreen = hex(0x2FBF71), cOrange = hex(0xF29D38), cBlue = hex(0x3D7EFF), cRed = hex(0xE0453A);
constexpr uint16_t cKeyFace = hex(0x3A4658), cTick = hex(0xC8CCD2);
// 3-band waveform (rekordbox / CDJ "3Band"): lows blue, mids orange, highs white; the played side dimmed
// (colours from freedj-3000's renderer)
constexpr uint16_t cLow = hex(0x3A7BF0), cMid = hex(0xF0A030), cHigh = hex(0xF4F4F4);
constexpr uint16_t cLowP = hex(0x1A3870), cMidP = hex(0x6A4818), cHighP = hex(0x6E6E6E);

struct Layout {   // color layout rows, from the screen size
    int head, label, value, meter, wave0, wave1, over0, over1, btn0;
};
Layout colorLayout(int h)
{
    Layout l;
    l.head = 22;                     // deck badge, title
    l.label = l.head + 3;            // REMAIN / TEMPO / BPM labels
    l.value = l.label + 13;          // their values (24 px digits)
    l.meter = l.value + 27;          // beat-in-bar meter, 6 rows
    l.wave0 = l.meter + 9;
    l.btn0 = h - 44;
    l.over1 = l.btn0 - 6; l.over0 = l.over1 - 20;
    l.wave1 = l.over0 - 6;
    return l;
}

const char* name(const Model& m) { return m.track && m.track->name[0] ? m.track->name : "(no track)"; }

void fmtTime(char* b, size_t n, const Model& m, bool tenths)
{
    const nano::DeckStatus& s = m.st;
    if (!s.rate) { snprintf(b, n, tenths ? "--:--.-" : "--:--"); return; }
    uint32_t pos = (uint32_t)(s.posQ16 >> 16);
    uint32_t rem = pos < s.frames ? s.frames - pos : 0;
    uint32_t ds = (uint32_t)((uint64_t)rem * 10 / s.rate);            // tenths of a second
    if (tenths) snprintf(b, n, "-%02u:%02u.%u", (unsigned)(ds / 600), (unsigned)(ds / 10 % 60), (unsigned)(ds % 10));
    else snprintf(b, n, "-%02u:%02u", (unsigned)(ds / 600), (unsigned)(ds / 10 % 60));
}

void fmtTempo(char* b, size_t n, const Model& m)
{
    int p = m.st.pitchPermille, a = p < 0 ? -p : p;
    snprintf(b, n, "%c%d.%d%%", p < 0 ? '-' : '+', a / 10, a % 10);
}

const char* transport(const Model& m)
{
    if (m.st.scratching) return "SCR";
    if (m.st.cueing) return "CUE";
    return m.st.playing ? "PLAY" : "PAUSE";
}

uint8_t peakAt(const nano::Track& t, int64_t b0, int nb)
{
    uint8_t v = 0;
    for (int64_t b = b0 < 0 ? 0 : b0; b < b0 + nb && b < (int64_t)t.peaksLen; b++)
        if (t.peak((uint32_t)b) > v) v = t.peak((uint32_t)b);
    return v;
}

// Zoomed waveform round the playhead (centre column); `msPerCol` sets the zoom.
void wave(Surface& s, const Model& m, int x0, int y0, int w, int h, int msPerCol, bool color)
{
    if (!s.rows(y0, h)) return;
    const int mid = y0 + h / 2, cx = x0 + w / 2;
    if (m.track && m.track->peaks && m.st.rate) {
        const nano::Track& t = *m.track;
        const int per = msPerCol / kPeakMs < 1 ? 1 : msPerCol / kPeakMs;
        const int64_t pb = (m.st.posQ16 >> 16) / t.peakBucket;
        for (int c = 0; c < w; c++) {
            const int64_t b = pb + (int64_t)(c - w / 2) * per;
            const int a = peakAt(t, b, per) * (h / 2) / 255;
            if (a) s.fill(x0 + c, mid - a, 1, 2 * a, color && c < w / 2 ? kPlayed : kWave);
        }
        const int64_t cb = (int64_t)m.st.cue / t.peakBucket;
        const int cc = (int)((cb - pb) / per) + w / 2;
        if (cc >= 0 && cc < w) {
            if (color) s.fill(x0 + cc - 3, y0, 7, 4, kCueC), vline(s, x0 + cc, y0, h, kCueC);
            else s.fill(x0 + cc - 1, y0, 3, 2, kText);
        }
    } else {
        hline(s, x0, mid, w, kDim);
    }
    s.fill(cx, y0, color ? 2 : 1, h, kHead);
}

// The whole track across the width, with the position.
void overview(Surface& s, const Model& m, int x0, int y0, int w, int h, bool color)
{
    if (!s.rows(y0, h)) return;
    if (!m.track || !m.st.frames) { frame(s, x0, y0, w, h, kDim); return; }
    const nano::Track& t = *m.track;
    const int px = (int)((m.st.posQ16 >> 16) * w / m.st.frames);
    if (color && t.peaks) {
        const int mid = y0 + h / 2;
        for (int c = 0; c < w; c++) {
            const int64_t b0 = (int64_t)c * t.peaksLen / w, b1 = (int64_t)(c + 1) * t.peaksLen / w;
            const int a = peakAt(t, b0, (int)(b1 - b0 < 1 ? 1 : b1 - b0)) * (h / 2) / 255;
            if (a) s.fill(x0 + c, mid - a, 1, 2 * a, c < px ? kDim : kWave);
        }
        vline(s, x0 + (int)((uint64_t)m.st.cue * w / m.st.frames), y0, h, kCueC);
        s.fill(x0 + px, y0, 2, h, kText);
    } else {
        frame(s, x0, y0, w, h, kText);
        s.fill(x0, y0, px, h, kText);
    }
}

void list(Surface& s, const Model& m, int y0, int rowH, int scale, bool color)
{
    const int rows = (s.height() - y0) / rowH;
    int first = m.selected - rows / 2;
    if (first > m.tracks - rows) first = m.tracks - rows;
    if (first < 0) first = 0;
    s.fill(0, y0, s.width(), s.height() - y0, color ? cBg : kBg);
    for (int r = 0; r < rows && first + r < m.tracks; r++) {
        const int i = first + r, y = y0 + r * rowH;
        if (!s.rows(y, rowH)) continue;
        const bool sel = i == m.selected, loaded = i == m.st.track;
        if (sel) s.fill(0, y, s.width(), rowH, color ? cKeyFace : kText);
        char b[48];
        snprintf(b, sizeof b, "%c%02d %s", loaded ? '>' : ' ', i + 1, m.nameOf ? m.nameOf(i, m.ctx) : "");
        if (color) text(s, 6, y + (rowH - ascent(kHelvR12)) / 2, b, loaded ? cOrange : cText, kHelvR12);
        else text(s, 2, y + (rowH - 8 * scale) / 2, b, sel ? kBg : kText, scale);
    }
}

void mono32(Surface& s, const Model& m)
{
    char b[32], t[16], p[12];
    if (m.browsing) { list(s, m, 0, 8, 1, false); return; }
    snprintf(b, sizeof b, "%02d %s", m.st.track + 1, name(m));
    text(s, 0, 0, b, kText);
    wave(s, m, 0, 8, s.width(), 16, 40, false);
    fmtTime(t, sizeof t, m, false); fmtTempo(p, sizeof p, m);
    snprintf(b, sizeof b, "%s %s%s", t, p, m.st.keylock ? " K" : "");
    text(s, 0, 25, b, kText);
    text(s, s.width() - textWidth(transport(m)), 25, transport(m), kText);
}

void mono64(Surface& s, const Model& m)
{
    char b[32], t[16], p[12];
    snprintf(b, sizeof b, "%02d/%02d %s", m.st.track + 1, m.tracks, name(m));
    text(s, 0, 0, b, kText);
    if (m.browsing) { list(s, m, 9, 9, 1, false); return; }
    fmtTime(t, sizeof t, m, true); fmtTempo(p, sizeof p, m);
    text(s, 0, 10, t, kText);
    text(s, s.width() - textWidth(p), 10, p, kText);
    wave(s, m, 0, 19, s.width(), 30, 40, false);
    overview(s, m, 0, 51, s.width(), 4, false);
    text(s, 0, 57, transport(m), kText);
    if (m.st.keylock) text(s, 40, 57, "KEY", kText);
    snprintf(b, sizeof b, "+-%d", m.st.rangePct);
    text(s, s.width() - textWidth(b), 57, b, kText);
}

// ---- color (CDJ-3000 style) -----------------------------------------------------

int64_t framesPerBeatQ16(const nano::Track& t)     // 0: no beat grid
{
    return t.bpmX100 ? ((int64_t)t.pcm.rate * 6000 << 16) / t.bpmX100 : 0;
}

void tri(Surface& s, int cx, int y, int h, bool down, uint16_t c)   // a small marker triangle
{
    for (int i = 0; i < h; i++) { const int w = down ? h - i : i + 1; s.fill(cx - w + 1, y + i, 2 * w - 1, 1, c); }
}

// One column of the 3-band waveform, layered as the CDJ-3000 (and freedj-3000) draws it:
// lows outermost, mids over them, highs innermost in white.
void bands3(Surface& s, int x, int mid, int half, const uint8_t* p, bool played)
{
    const int a[3] = {p[0] * half / 255, p[1] * half / 255, p[2] * half / 255};
    const uint16_t c[3] = {played ? cLowP : cLow, played ? cMidP : cMid, played ? cHighP : cHigh};
    for (int i = 0; i < 3; i++) if (a[i]) s.fill(x, mid - a[i], 1, 2 * a[i], c[i]);
}

void maxBands(const nano::Track& t, int64_t b0, int nb, uint8_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    for (int64_t b = b0 < 0 ? 0 : b0; b < b0 + nb && b < (int64_t)t.peaksLen; b++) {
        if (!t.bands) { const uint8_t v = t.peaks[b]; if (v > out[0]) out[0] = v; continue; }
        const uint8_t* p = t.peaks + 3 * b;
        for (int k = 0; k < 3; k++) if (p[k] > out[k]) out[k] = p[k];
    }
}

void waveColor(Surface& s, const Model& m, int y0, int w, int h)
{
    if (!s.rows(y0, h)) return;
    const int mid = y0 + h / 2, cx = w / 2;
    if (m.track && m.track->peaks && m.st.rate) {
        const nano::Track& t = *m.track;
        const int64_t pos = m.st.posQ16 >> 16, pb = pos / t.peakBucket;
        for (int c = 0; c < w; c++) {                 // 10 ms per column
            uint8_t p[3];
            maxBands(t, pb + (c - cx), 1, p);
            bands3(s, c, mid, h / 2 - 1, p, c < cx);
        }
        if (const int64_t fpb = framesPerBeatQ16(t)) { // beat grid: ticks top and bottom, bars in red
            const int64_t fpc = t.peakBucket;          // frames per column
            const int64_t f0 = pos - cx * fpc, f1 = pos + (w - cx) * fpc;
            int64_t k = (((f0 - (int64_t)t.firstBeat) << 16) / fpb) - 1;
            for (;; k++) {
                const int64_t bf = (int64_t)t.firstBeat + ((k * fpb) >> 16);
                if (bf > f1) break;
                const int x = (int)((bf - pos) / fpc) + cx;
                if (x < 0 || x >= w) continue;
                const bool bar = (k % 4 + 4) % 4 == 0;
                const int len = bar ? 7 : 4;
                const uint16_t c = bar ? cRed : cTick;
                s.fill(x, y0, 1, len, c); s.fill(x, y0 + h - len, 1, len, c);
            }
        }
        const int cc = (int)(((int64_t)m.st.cue - pos) / (int64_t)t.peakBucket) + cx;
        if (cc >= 0 && cc < w) tri(s, cc, y0, 6, true, cOrange);
    } else {
        hline(s, 0, mid, w, cPanel);
    }
    s.fill(cx - 1, y0, 2, h, cText);                  // playhead
}

void overviewColor(Surface& s, const Model& m, int y0, int w, int h)
{
    if (!s.rows(y0, h)) return;
    s.fill(0, y0, w, h, cPanel);
    if (!m.track || !m.st.frames || !m.track->peaks) return;
    const nano::Track& t = *m.track;
    const int px = (int)((m.st.posQ16 >> 16) * w / m.st.frames), mid = y0 + h / 2;
    for (int c = 0; c < w; c++) {
        const int64_t b0 = (int64_t)c * t.peaksLen / w, b1 = (int64_t)(c + 1) * t.peaksLen / w;
        uint8_t p[3];
        maxBands(t, b0, (int)(b1 - b0 < 1 ? 1 : b1 - b0), p);
        bands3(s, c, mid, h / 2 - 1, p, c < px);
    }
    tri(s, (int)((uint64_t)m.st.cue * w / m.st.frames), y0 + h - 5, 5, false, cOrange);
    s.fill(px, y0, 1, h, cText);
}

void meter(Surface& s, const Model& m, int y, int w)
{
    if (!s.rows(y, 6)) return;
    int lit = -1;
    if (m.track && m.st.rate)
        if (const int64_t fpb = framesPerBeatQ16(*m.track)) {
            const int64_t k = (((m.st.posQ16 >> 16) - (int64_t)m.track->firstBeat) << 16) / fpb;
            lit = (int)((k % 4 + 4) % 4);
        }
    const int gap = 4, bw = (w - 12 - 3 * gap) / 4;
    for (int i = 0; i < 4; i++) s.fill(6 + i * (bw + gap), y, bw, 6, i == lit ? (lit == 0 ? cRed : cOrange) : cPanel);
}

void key(Surface& s, int i, int n, int y0, int h, const char* label, uint16_t accent, bool on)
{
    if (!s.rows(y0, h)) return;
    const int w = s.width() / n, x = i * w + 3, kw = w - 6;
    s.fill(x, y0, kw, h, on ? accent : cKeyFace);
    s.fill(x, y0, kw, 1, on ? cText : cDim);          // a lit top edge, as on the keys
    text(s, x + (kw - textWidth(label, kHelvR14)) / 2, y0 + (h - ascent(kHelvR14)) / 2, label, on ? cBg : accent, kHelvR14);
}

void color(Surface& s, const Model& m)
{
    const Layout l = colorLayout(s.height());
    const int W = s.width();
    char b[48], t[16], p[12];
    s.fill(0, 0, W, s.height(), cBg);

    // header: deck badge, title, track count and fps
    s.fill(0, 0, W, l.head, cPanel);
    s.fill(3, 3, l.head - 6, l.head - 6, cBlue);
    text(s, 3 + (l.head - 6 - textWidth("1", kHelvR12)) / 2, 3 + (l.head - 6 - ascent(kHelvR12)) / 2, "1", cText, kHelvR12);
    text(s, l.head + 4, (l.head - ascent(kHelvR14)) / 2, name(m), cText, kHelvR14);
    snprintf(b, sizeof b, "%d/%d", m.st.track + 1, m.tracks);
    if (m.fps >= 0) snprintf(b + strlen(b), sizeof b - strlen(b), "  %d fps", m.fps);
    s.fill(W - textWidth(b, kHelvR10) - 12, 0, textWidth(b, kHelvR10) + 12, l.head, cPanel);
    text(s, W - textWidth(b, kHelvR10) - 6, (l.head - ascent(kHelvR10)) / 2, b, cDim, kHelvR10);
    if (m.browsing) { list(s, m, l.head, 24, 2, true); return; }

    // REMAIN | TEMPO (MT) | BPM
    const int tx = W * 45 / 100;
    text(s, 6, l.label, "REMAIN", cDim, kHelvR10);
    text(s, tx, l.label, "TEMPO", cDim, kHelvR10);
    text(s, W - textWidth("BPM", kHelvR10) - 6, l.label, "BPM", cDim, kHelvR10);
    fmtTime(t, sizeof t, m, true);
    text(s, 6, l.value, t, cText, kHelvR24Num);
    fmtTempo(p, sizeof p, m);
    const int px = text(s, tx, l.value + 1, p, cText, kHelvR14);
    snprintf(b, sizeof b, "+-%d", m.st.rangePct);
    text(s, tx, l.value + 17, b, cDim, kHelvR10);
    if (m.st.keylock) {                               // MASTER TEMPO, as the CDJ calls keylock
        const int bx = px + 6, bw = textWidth("MT", kHelvR10) + 6;
        s.fill(bx, l.value + 1, bw, 13, cRed);
        text(s, bx + 3, l.value + 2, "MT", cText, kHelvR10);
    }
    if (m.track && m.track->bpmX100) {
        const int64_t bpm10 = (int64_t)m.track->bpmX100 * (1000 + m.st.pitchPermille) / 10000;   // tenths, at tempo
        snprintf(b, sizeof b, "%d.%d", (int)(bpm10 / 10), (int)(bpm10 % 10));
    } else {
        snprintf(b, sizeof b, "--.-");
    }
    text(s, W - textWidth(b, kHelvR24Num) - 6, l.value, b, cText, kHelvR24Num);

    meter(s, m, l.meter, W);
    waveColor(s, m, l.wave0, W, l.wave1 - l.wave0);
    overviewColor(s, m, l.over0, W, l.over1 - l.over0);
    const int bh = s.height() - l.btn0 - 4;
    key(s, 0, 4, l.btn0, bh, "CUE", cOrange, m.st.cueing);
    key(s, 1, 4, l.btn0, bh, m.st.playing ? "PAUSE" : "PLAY", cGreen, m.st.playing);
    key(s, 2, 4, l.btn0, bh, "MT", cRed, m.st.keylock);
    key(s, 3, 4, l.btn0, bh, "LIST", cText, m.browsing);
}

} // namespace

int sections(int w, int h, int* edges, int max)
{
    if (max < 2) return 0;
    if (w < 240 || h <= 64) { edges[0] = 0; edges[1] = h; return 2; }
    const Layout l = colorLayout(h);
    const int e[] = {0, l.wave0, l.wave1, l.btn0, h};
    int n = 0;
    for (int v : e) if (n < max) edges[n++] = v;
    return n;
}

void draw(Surface& s, const Model& m)
{
    s.fill(0, 0, s.width(), s.height(), kBg);
    if (s.height() <= 32) mono32(s, m);
    else if (s.mono() || s.width() < 240) mono64(s, m);
    else color(s, m);
}

int Touch::feed(const Model& m, int w, int h, bool down, int x, int y, deck::Event* out, int max)
{
    using deck::Type;
    int n = 0;
    auto emit = [&](Type t, bool pressed, int16_t delta = 0) {
        if (n >= max) return;
        deck::Event e; e.type = t; e.deck = deck::DeckA; e.pressed = pressed; e.delta = delta;
        out[n++] = e;
    };
    const Layout l = colorLayout(h);
    if (down && active_ == None) {                     // new touch: what's under it?
        if (y >= l.btn0) {
            static const Hit kRow[4] = {Cue, Play, Key, Next};
            active_ = kRow[x * 4 / w < 4 ? x * 4 / w : 3];
            if (active_ == Cue) emit(Type::Cue, true);
            else if (active_ == Play) emit(Type::PlayPause, true);
            else if (active_ == Key) emit(Type::KeyLock, true);
            else emit(Type::Browse, true);             // the app toggles the list
        } else if (m.browsing && y >= l.head) {
            active_ = List;
            const int row = (y - l.head) / 24, rows = (h - l.head) / 24;
            int first = m.selected - rows / 2;
            if (first > m.tracks - rows) first = m.tracks - rows;
            if (first < 0) first = 0;
            if (first + row < m.tracks) emit(Type::Load, true, (int16_t)(first + row));
        } else if (y >= l.wave0 && y < l.over1) {
            active_ = Wave; lastX_ = x; acc_ = 0;
            emit(Type::JogTouch, true);
        } else {
            active_ = List;                            // dead zone: swallow until release
        }
        return n;
    }
    if (down && active_ == Wave) {                     // drag: grab the waveform (left = forward)
        acc_ += (lastX_ - x) * 5;                      // 2.5 jog ticks per pixel (10 ms/px at 4 ms/tick)
        lastX_ = x;
        if (acc_ / 2) { emit(Type::Jog, false, (int16_t)(acc_ / 2)); acc_ %= 2; }
        return n;
    }
    if (!down && active_ != None) {
        if (active_ == Cue) emit(Type::Cue, false);
        else if (active_ == Play) emit(Type::PlayPause, false);
        else if (active_ == Wave) emit(Type::JogTouch, false);
        active_ = None;
    }
    return n;
}

} // namespace ui
