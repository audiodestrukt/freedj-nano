// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the portable core. Self-asserting; also writes listening
// files (out/*.wav) and screenshots of every UI layout (out/*.ppm).
//   cmake -S tests -B build/tests && cmake --build build/tests && build/tests/nano_tests

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "nano/deck.h"
#include "nano/dj2go.h"
#include "nano/gfx.h"
#include "nano/library.h"
#include "nano/midi.h"
#include "nano/stretch.h"
#include "nano/ui.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace nano;

// ---- helpers ---------------------------------------------------------------

static std::vector<int16_t> sine(uint32_t rate, float hz, float secs, int ch, float amp = 10000)
{
    std::vector<int16_t> v((size_t)(rate * secs) * ch);
    for (size_t i = 0; i < v.size() / ch; i++)
        for (int c = 0; c < ch; c++) v[i * ch + c] = (int16_t)(amp * sinf(6.2831853f * hz * i / rate));
    return v;
}

// Something music-like: a decaying "kick" every 0.5 s over a chord, stereo.
static std::vector<int16_t> beat(uint32_t rate, float secs)
{
    std::vector<int16_t> v((size_t)(rate * secs) * 2);
    for (size_t i = 0; i < v.size() / 2; i++) {
        float t = (float)i / rate, tb = fmodf(t, 0.5f);
        float kick = 9000 * expf(-tb * 18) * sinf(6.2831853f * (50 + 120 * expf(-tb * 30)) * tb);
        float pad = 1500 * (sinf(6.2831853f * 220 * t) + sinf(6.2831853f * 277.2f * t) + sinf(6.2831853f * 329.6f * t));
        v[2 * i] = (int16_t)(kick + pad);
        v[2 * i + 1] = (int16_t)(kick + 0.8f * pad);
    }
    return v;
}

static int zeroCrossings(const int32_t* st, int n)   // on the left channel
{
    int z = 0;
    for (int i = 1; i < n; i++) if ((st[2 * (i - 1)] < 0) != (st[2 * i] < 0)) z++;
    return z;
}

static double rms(const int32_t* st, int n)
{
    double s = 0; for (int i = 0; i < n; i++) s += (double)st[2 * i] * st[2 * i];
    return sqrt(s / n);
}

static int maxStep(const int16_t* st, int n)
{
    int m = 0; for (int i = 1; i < n; i++) { int d = abs(st[2 * i] - st[2 * (i - 1)]); if (d > m) m = d; } return m;
}

static void writeWav(const char* path, const int16_t* st, int frames, uint32_t rate)
{
    FILE* f = fopen(path, "wb"); if (!f) return;
    uint32_t data = frames * 4, riff = 36 + data, fmt = 16, byteRate = rate * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(st, 4, frames, f); fclose(f);
}

// A pack image in memory, in the format tools/pack_tracks.py writes.
struct PackTrack { std::string name; std::vector<int16_t> pcm; uint32_t rate; uint8_t ch; };
static std::vector<uint8_t> makePack(const std::vector<PackTrack>& ts)
{
    std::vector<uint8_t> img(8 + 64 * ts.size(), 0);
    memcpy(img.data(), "FDJN", 4); img[4] = 1; img[6] = (uint8_t)ts.size();
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) img[at + i] = (uint8_t)(v >> (8 * i)); };
    for (size_t k = 0; k < ts.size(); k++) {
        const PackTrack& t = ts[k];
        const uint32_t frames = (uint32_t)(t.pcm.size() / t.ch), bucket = t.rate / 100;
        while (img.size() % 4) img.push_back(0);
        const uint32_t off = (uint32_t)img.size();
        const uint8_t* p = (const uint8_t*)t.pcm.data();
        img.insert(img.end(), p, p + t.pcm.size() * 2);
        const uint32_t poff = (uint32_t)img.size(), plen = (frames + bucket - 1) / bucket;
        for (uint32_t b = 0; b < plen; b++) {
            int m = 0;
            for (uint32_t f = b * bucket; f < (b + 1) * bucket && f < frames; f++)
                for (int c = 0; c < t.ch; c++) m = std::max(m, abs((int)t.pcm[f * t.ch + c]));
            img.push_back((uint8_t)std::min(255, m >> 7));
        }
        const size_t e = 8 + 64 * k;
        strncpy((char*)&img[e], t.name.c_str(), 39);
        put32(e + 40, off); put32(e + 44, frames); put32(e + 48, t.rate); img[e + 52] = t.ch;
        put32(e + 56, poff); put32(e + 60, plen);
    }
    return img;
}

static void writePpm(const char* path, const gfx::Frame& f)
{
    FILE* o = fopen(path, "wb"); if (!o) return;
    fprintf(o, "P6 %d %d 255\n", f.width(), f.height());
    for (int i = 0; i < f.width() * f.height(); i++) {
        uint16_t c = f.pixels()[i];
        uint8_t rgb3[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
        fwrite(rgb3, 1, 3, o);
    }
    fclose(o);
}

static deck::Event ev(deck::Type t, bool pressed = true, int16_t delta = 0, float pos = 0)
{
    deck::Event e; e.type = t; e.deck = deck::DeckA; e.pressed = pressed; e.delta = delta; e.position = pos; return e;
}

// ---- tests -------------------------------------------------------------------

static void testMidi()
{
    MidiParser p; MidiMsg m; int got = 0;
    const uint8_t s[] = {0x90, 0x3B, 0xF8, 0x7F,        // note on, clock in the middle
                         0x3B, 0x00,                    // running status: note off (vel 0)
                         0xF0, 0x01, 0x02, 0xF7,        // sysex: skipped
                         0xB0, 0x19, 0x01,              // jog CC
                         0xC0, 0x05};                   // program change: 1 data byte
    MidiMsg all[8];
    for (uint8_t b : s) if (p.feed(b, m)) all[got++] = m;
    CHECK(got == 4, "got %d messages", got);
    CHECK(all[0].status == 0x90 && all[0].d1 == 0x3B && all[0].d2 == 0x7F, "first");
    CHECK(all[1].status == 0x90 && all[1].d2 == 0, "running status");
    CHECK(all[2].status == 0xB0 && all[2].d1 == 0x19, "cc after sysex");
    CHECK(all[3].status == 0xC0 && all[3].d1 == 5, "program change");

    dj2go::Decoder d; deck::Event e[2];
    int n = d.decode(0x90, 0x3B, 0x7F, e, 2);
    CHECK(n == 1 && e[0].type == deck::Type::PlayPause && e[0].pressed && e[0].deck == deck::DeckA, "dj2go play");
    n = d.decode(0xB0, 0x19, 0x7E, e, 2);
    CHECK(n == 1 && e[0].type == deck::Type::Jog && e[0].delta == -2, "dj2go jog -2");
}

static void testLibrary()
{
    auto img = makePack({{"Sine", sine(22050, 440, 2, 1), 22050, 1}, {"Beat", beat(44100, 3), 44100, 2}});
    Library lib;
    CHECK(lib.open(img.data(), img.size()) && lib.count() == 2, "open");
    Track t;
    CHECK(lib.get(1, t) && !strcmp(t.name, "Beat") && t.pcm.channels == 2 && t.pcm.frames == 132300, "track 2");
    CHECK(t.peakBucket == 441 && t.peaksLen == 300, "peaks %u x %u", t.peakBucket, t.peaksLen);
    CHECK(!lib.get(2, t), "out of range");
    img[0] = 'X';
    CHECK(!lib.open(img.data(), img.size()) && lib.count() == 0, "bad magic");
}

static void testStretch()
{
    const uint32_t rate = 44100;
    auto src = sine(rate, 441, 8, 2);
    Pcm pcm{src.data(), (uint32_t)(src.size() / 2), rate, 2};
    StretchWindow w; w.build(stretchHop(rate));
    const int N = rate * 3;
    std::vector<int32_t> out(N * 2);
    double inRms = 10000 / sqrt(2.0);
    for (float sp : {0.8f, 1.25f, 0.5f, 2.0f}) {
        Stretch s; s.begin(&w); s.reset(rate / 2);
        const int32_t q = (int32_t)(sp * kOne);
        for (int k = 0; k < N; k += 256) s.render(pcm, q, &out[2 * k], std::min(256, N - k));
        const double moved = (double)(s.posQ16() >> 16) - rate / 2;
        CHECK(fabs(moved - N * sp) < 600, "speed %.2f moved %.0f frames, want %.0f", sp, moved, N * sp);
        const int z = zeroCrossings(out.data() + 2 * 4096, N - 4096), want = (int)(2.0 * 441 * (N - 4096) / rate);
        CHECK(abs(z - want) < want / 50, "speed %.2f: %d zero crossings, want ~%d (pitch kept)", sp, z, want);
        const double r = rms(out.data() + 2 * 4096, N - 4096);
        CHECK(r > inRms * 0.85 && r < inRms * 1.15, "speed %.2f rms %.0f vs %.0f", sp, r, inRms);
        std::vector<int16_t> o16(N * 2);
        for (int i = 0; i < 2 * N; i++) o16[i] = sat16(out[i]);
        const int step = maxStep(o16.data() + 2 * 4096, N - 4096);
        CHECK(step < 1300, "speed %.2f: max sample step %d (clicks?)", sp, step);   // sine's own max step ~630
    }
    // varispeed doubles the pitch at 2x
    int64_t pos = (int64_t)(rate / 2) << 16;
    varispeed(pcm, pos, 2 * kOne, out.data(), N);
    const int z = zeroCrossings(out.data(), N), want = (int)(2.0 * 882 * N / rate);
    CHECK(abs(z - want) < want / 50, "varispeed 2x: %d crossings, want ~%d", z, want);
    CHECK((pos >> 16) == rate / 2 + 2 * N, "varispeed position");

    // listening files: the beat at 0.8 and 1.25 with keylock
    auto b = beat(rate, 10);
    Pcm bp{b.data(), (uint32_t)(b.size() / 2), rate, 2};
    for (float sp : {0.8f, 1.25f}) {
        Stretch s; s.begin(&w); s.reset(0);
        const int M = rate * 6;
        std::vector<int32_t> o(M * 2); std::vector<int16_t> o16(M * 2);
        for (int k = 0; k < M; k += 256) s.render(bp, (int32_t)(sp * kOne), &o[2 * k], std::min(256, M - k));
        for (int i = 0; i < 2 * M; i++) o16[i] = sat16(o[i]);
        char path[64]; snprintf(path, sizeof path, "out/keylock_%03d.wav", (int)(sp * 100));
        writeWav(path, o16.data(), M, rate);
    }
}

static void testDeck()
{
    const uint32_t rate = 22050;
    auto img = makePack({{"Sine", sine(rate, 441, 4, 1), rate, 1}, {"Beat", beat(rate, 6), rate, 2}});
    Library lib; lib.open(img.data(), img.size());
    Deck d(lib);
    std::vector<int16_t> out(Deck::kBlock * 2), all;
    auto run = [&](int blocks) { for (int i = 0; i < blocks; i++) { d.render(out.data(), Deck::kBlock); all.insert(all.end(), out.begin(), out.end()); } };
    auto posF = [&] { return (int)(d.status().posQ16 >> 16); };

    d.post(ev(deck::Type::Load, true, 0)); run(1);
    CHECK(d.status().track == 0 && d.status().rate == rate && !d.status().playing, "loaded");
    CHECK(d.rate() == rate, "rate");
    d.post(ev(deck::Type::PlayPause)); run(10);
    CHECK(d.status().playing && abs(posF() - 10 * Deck::kBlock) <= 1, "playing, pos %d", posF());
    d.post(ev(deck::Type::Cue)); run(2);                           // CUE while playing: back to cue (0), paused
    CHECK(!d.status().playing && posF() == 0, "cue: pos %d", posF());
    d.post(ev(deck::Type::Cue)); run(4);                           // hold CUE: preview
    CHECK(d.status().cueing && posF() > 0, "preview");
    d.post(ev(deck::Type::Cue, false)); run(2);
    CHECK(!d.status().cueing && posF() == 0, "preview released: back to cue");

    d.post(ev(deck::Type::TempoFader, false, 0, 1.0f));            // +8 %
    d.post(ev(deck::Type::PlayPause)); run(1);
    CHECK(d.status().pitchPermille == 80 && abs(d.status().speedQ16 - (kOne + kOne * 8 / 100)) < 8, "tempo +8 %%");
    CHECK(!d.status().stretching, "no keylock yet");
    d.post(ev(deck::Type::KeyLock)); run(4);
    CHECK(d.status().keylock && d.status().stretching, "keylock engine on");
    int p0 = posF(); run(20);
    const int moved = posF() - p0, want = 20 * Deck::kBlock * 108 / 100;
    CHECK(abs(moved - want) < 300, "keylock speed: moved %d, want %d", moved, want);

    d.post(ev(deck::Type::JogTouch)); run(3);                      // hand on: stops
    CHECK(d.status().scratching && abs(d.status().speedQ16) < kOne / 8, "touch holds, speed %d", d.status().speedQ16);
    p0 = posF();
    for (int i = 0; i < 10; i++) { d.post(ev(deck::Type::Jog, false, 3)); run(1); }
    CHECK(posF() > p0, "scratch forward");
    d.post(ev(deck::Type::JogTouch, false)); run(4);
    CHECK(!d.status().scratching && d.status().playing, "release: plays on");

    // No clicks anywhere in that session: the sine is mono 441 Hz at 22.05 kHz (max step ~1260).
    const int step = maxStep(all.data(), (int)all.size() / 2);
    CHECK(step < 2600, "max step %d over the deck session", step);
    writeWav("out/deck_session.wav", all.data(), (int)all.size() / 2, rate);

    d.post(ev(deck::Type::Load, true, 1)); d.post(ev(deck::Type::PlayPause)); run(1);
    CHECK(d.status().track == 1 && d.status().playing, "load 2");
    for (int i = 0; i < 6 * (int)rate / Deck::kBlock + 20; i++) d.render(out.data(), Deck::kBlock);
    CHECK(!d.status().playing, "stops at the end");
}

static const char* nameOf(int i, void* ctx) { Track t; return ((Library*)ctx)->get(i, t) ? t.name : ""; }

static void testUi()
{
    auto img = makePack({{"Kick Pad Loop", beat(22050, 20), 22050, 2}, {"Sine 441", sine(22050, 441, 5, 1), 22050, 1},
                         {"Third Track Name Long", beat(22050, 3), 22050, 2}});
    Library lib; lib.open(img.data(), img.size());
    Track t; lib.get(0, t);
    ui::Model m;
    m.track = &t; m.tracks = lib.count(); m.nameOf = nameOf; m.ctx = &lib;
    m.st.track = 0; m.st.rate = 22050; m.st.frames = t.pcm.frames;
    m.st.posQ16 = (int64_t)(22050 * 7) << 16; m.st.cue = 22050 * 6;
    m.st.playing = true; m.st.keylock = true; m.st.pitchPermille = 25;

    struct S { const char* name; int w, h; bool mono; } screens[] = {
        {"oled_128x32", 128, 32, true}, {"oled_128x64", 128, 64, true},
        {"tft_320x240", 320, 240, false}, {"tft_480x320", 480, 320, false}};
    for (const S& s : screens) {
        for (int browse = 0; browse < 2; browse++) {
            std::vector<uint16_t> px(s.w * s.h);
            gfx::Frame f(px.data(), s.w, s.h, s.h, s.mono);
            m.browsing = browse; m.selected = 1;
            ui::draw(f, m);
            int lit = 0; for (uint16_t c : px) lit += c != 0;
            CHECK(lit > s.w * s.h / 50, "%s: %d lit pixels", s.name, lit);
            char path[64]; snprintf(path, sizeof path, "out/%s%s.ppm", s.name, browse ? "_list" : "");
            writePpm(path, f);
        }
        // band rendering (the TFT backend) gives the same picture as a full frame
        if (!s.mono) {
            m.browsing = false;
            std::vector<uint16_t> full(s.w * s.h), band(s.w * 16), joined(s.w * s.h);
            gfx::Frame ff(full.data(), s.w, s.h, s.h); ui::draw(ff, m);
            gfx::Frame bf(band.data(), s.w, s.h, 16);
            for (int y = 0; y < s.h; y += 16) {
                bf.setBand(y); ui::draw(bf, m);
                memcpy(&joined[y * s.w], band.data(), sizeof(uint16_t) * s.w * std::min(16, s.h - y));
            }
            CHECK(full == joined, "%s: banded render matches", s.name);
        }
    }

    // touch: PLAY button, then a drag on the waveform
    ui::Touch tc; deck::Event e[4];
    int n = tc.feed(m, 320, 240, true, 120, 220, e, 4);
    CHECK(n == 1 && e[0].type == deck::Type::PlayPause && e[0].pressed, "touch PLAY");
    n = tc.feed(m, 320, 240, false, 120, 220, e, 4);
    CHECK(n == 1 && e[0].type == deck::Type::PlayPause && !e[0].pressed, "release PLAY");
    n = tc.feed(m, 320, 240, true, 160, 120, e, 4);
    CHECK(n == 1 && e[0].type == deck::Type::JogTouch && e[0].pressed, "touch waveform");
    n = tc.feed(m, 320, 240, true, 150, 120, e, 4);
    CHECK(n == 1 && e[0].type == deck::Type::Jog && e[0].delta == 25, "drag left 10 px: +25 ticks (%d)", n ? e[0].delta : 0);
    n = tc.feed(m, 320, 240, false, 150, 120, e, 4);
    CHECK(n == 1 && e[0].type == deck::Type::JogTouch && !e[0].pressed, "release waveform");
}

// nano_tests --pack tracks.bin: list a pack the way the firmware will read it
static int listPack(const char* path)
{
    FILE* f = fopen(path, "rb"); if (!f) { perror(path); return 1; }
    std::vector<uint8_t> img; uint8_t b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) img.insert(img.end(), b, b + n);
    fclose(f);
    Library lib;
    if (!lib.open(img.data(), img.size())) { printf("%s: not a track pack\n", path); return 1; }
    for (int i = 0; i < lib.count(); i++) {
        Track t;
        if (!lib.get(i, t)) { printf("%02d: bad entry\n", i + 1); return 1; }
        printf("%02d %s: %u Hz x%u, %.1f s, %u peaks of %u frames\n", i + 1, t.name, t.pcm.rate, t.pcm.channels,
               (double)t.pcm.frames / t.pcm.rate, t.peaksLen, t.peakBucket);
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && !strcmp(argv[1], "--pack")) return listPack(argv[2]);
    if (system("mkdir -p out")) {}
    testMidi();
    testLibrary();
    testStretch();
    testDeck();
    testUi();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
