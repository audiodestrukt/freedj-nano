// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nano/ui.h"

#include <stdio.h>
#include <string.h>

using namespace gfx;

namespace ui {

namespace {

constexpr uint16_t kBg = kBlack, kText = kWhite;
constexpr uint16_t kDim = rgb(90, 90, 90), kBar = rgb(40, 40, 48);
constexpr uint16_t kWave = rgb(0, 170, 255), kPlayed = rgb(0, 80, 140);
constexpr uint16_t kHead = rgb(255, 40, 40), kCueC = rgb(255, 150, 0);
constexpr uint16_t kPlayC = rgb(0, 200, 80), kKeyC = rgb(240, 200, 0);
constexpr int kPeakMs = 10;   // pack peaks are 10 ms buckets (tools/pack_tracks.py)

struct Layout {   // color layout rows, from the screen size
    int head, time, wave0, wave1, over0, over1, btn0;
};
Layout colorLayout(int h)
{
    Layout l;
    l.head = 24; l.time = l.head + 4;
    l.btn0 = h - 52;
    l.over1 = l.btn0 - 6; l.over0 = l.over1 - 22;
    l.wave0 = l.time + 32; l.wave1 = l.over0 - 6;
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
        if (t.peaks[b] > v) v = t.peaks[b];
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
    s.fill(0, y0, s.width(), s.height() - y0, kBg);
    for (int r = 0; r < rows && first + r < m.tracks; r++) {
        const int i = first + r, y = y0 + r * rowH;
        if (!s.rows(y, rowH)) continue;
        const bool sel = i == m.selected, loaded = i == m.st.track;
        if (sel) s.fill(0, y, s.width(), rowH, color ? kBar : kText);
        char b[48];
        snprintf(b, sizeof b, "%c%02d %s", loaded ? '>' : ' ', i + 1, m.nameOf ? m.nameOf(i, m.ctx) : "");
        text(s, 2, y + (rowH - 8 * scale) / 2, b, sel && !color ? kBg : kText, scale);
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

void button(Surface& s, int i, int n, int y0, int h, const char* label, uint16_t c, bool on)
{
    if (!s.rows(y0, h)) return;
    const int w = s.width() / n, x = i * w + 3;
    if (on) s.fill(x, y0, w - 6, h, c); else frame(s, x, y0, w - 6, h, c);
    text(s, x + (w - 6 - textWidth(label, 2)) / 2, y0 + (h - 16) / 2, label, on ? kBg : c, 2);
}

void color(Surface& s, const Model& m)
{
    const Layout l = colorLayout(s.height());
    const int W = s.width();
    char b[48], t[16], p[12];
    s.fill(0, 0, W, l.head, kBar);
    snprintf(b, sizeof b, "%02d/%02d %s", m.st.track + 1, m.tracks, name(m));
    text(s, 4, 4, b, kText, 2);
    if (m.browsing) { list(s, m, l.head, 24, 2, true); return; }
    fmtTime(t, sizeof t, m, true); fmtTempo(p, sizeof p, m);
    text(s, 4, l.time, t, kText, 3);
    text(s, W - textWidth(p, 2) - 4, l.time, p, kText, 2);
    snprintf(b, sizeof b, "+-%d%%", m.st.rangePct);
    text(s, W - textWidth(b) - 4, l.time + 18, b, kDim);
    wave(s, m, 0, l.wave0, W, l.wave1 - l.wave0, 10, true);
    overview(s, m, 0, l.over0, W, l.over1 - l.over0, true);
    const int bh = s.height() - l.btn0 - 4;
    button(s, 0, 4, l.btn0, bh, "CUE", kCueC, m.st.cueing || (!m.st.playing && m.st.track >= 0));
    button(s, 1, 4, l.btn0, bh, "PLAY", kPlayC, m.st.playing);
    button(s, 2, 4, l.btn0, bh, "KEY", kKeyC, m.st.keylock);
    button(s, 3, 4, l.btn0, bh, "LIST", kDim, m.browsing);
}

} // namespace

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
