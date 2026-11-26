// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// test_hifi: nano-hifi (hifi/hifi.c) against the C6 engine it was ported from (components/nano: Deck, Stretch),
// sample for sample, through the same session on the same tracks: stereo 44.1 kHz and mono 22.05 kHz; play,
// keylock in and out at +8 % and -12 %, unity (straight), CUE back and preview, a stop, and the engine switches
// and seeks crossfaded. Plus what hifi does alone: the window table, backwards, the gain.   make hifitest
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "hifi.h"
#include "nano/deck.h"
#include "nano/stretch.h"

static int s_fail, s_n;
#define CHECK(c, ...) do { s_n++; if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// a pack of one track (components/nano/include/nano/library.h, v1)
static std::vector<uint8_t> pack(const std::vector<int16_t>& pcm, uint32_t frames, uint32_t rate, uint8_t ch)
{
    std::vector<uint8_t> img(8 + 64, 0);
    memcpy(img.data(), "FDJN", 4);
    img[4] = 1; img[6] = 1;
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) img[at + i] = (uint8_t)(v >> (8 * i)); };
    const size_t e = 8;
    strcpy((char*)&img[e], "T");
    put32(e + 40, (uint32_t)img.size()); put32(e + 44, frames); put32(e + 48, rate); img[e + 52] = ch;
    const size_t off = img.size();
    img.resize(off + pcm.size() * 2);
    memcpy(&img[off], pcm.data(), pcm.size() * 2);
    return img;
}

// music-like: a bass line, a chord and noise bursts (so the keylock's search has something to choose)
static std::vector<int16_t> track(uint32_t rate, uint8_t ch, double secs)
{
    const uint32_t n = (uint32_t)(rate * secs);
    std::vector<int16_t> v(n * ch);
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        const double beat = fmod(t, 0.5);
        seed = seed * 1103515245u + 12345u;
        const double noise = ((int)(seed >> 16 & 0x7fff) - 16384) / 16384.0 * (beat < 0.03 ? 0.4 : 0.0);
        const double bass = 0.35 * sin(2 * M_PI * (55 + 27.5 * ((int)(t * 2) % 3)) * t);
        const double chord = 0.15 * (sin(2 * M_PI * 440 * t) + sin(2 * M_PI * 554.4 * t));
        for (uint8_t c = 0; c < ch; c++) v[i * ch + c] = (int16_t)(32767 * fmax(-1, fmin(1, bass + chord * (c ? 0.7 : 1) + noise)));
    }
    return v;
}

static deck::Event ev(deck::Type t, bool pressed = true, int16_t delta = 0, float pos = 0)
{
    deck::Event e; e.type = t; e.deck = deck::DeckA; e.pressed = pressed; e.delta = delta; e.position = pos; return e;
}

static void same_as_the_c6_engine(uint32_t rate, uint8_t ch)
{
    const double secs = 6;
    auto pcm = track(rate, ch, secs);
    const uint32_t frames = (uint32_t)(rate * secs);
    auto img = pack(pcm, frames, rate, ch);
    nano::Library lib;
    lib.open(img.data(), img.size());
    nano::Deck& ref = *new nano::Deck(lib);           // (atomics inside: not assignable; a fresh one a run)
    static hifi_deck h;
    const hifi_pcm p = {pcm.data(), frames, rate, ch};
    hifi_init(&h, &p);
    ref.post(ev(deck::Type::Load, true, 0));

    // The session: (block count, what happens first). The C6 deck's tempo fader is +-range around 0.5; hifi's
    // speed is set to what that makes (kOne + permille * kOne / 1000).
    int16_t a[2 * HIFI_BLOCK], b[2 * HIFI_BLOCK];
    long diffs = 0, firstAt = -1, total = 0;
    int block = 0, stretchedBlocks = 0;
    auto run = [&](int blocks) {
        for (int i = 0; i < blocks; i++, block++) {
            ref.render(a, HIFI_BLOCK);
            hifi_render(&h, b, HIFI_BLOCK);
            for (int k = 0; k < 2 * HIFI_BLOCK; k++, total++) if (a[k] != b[k]) { if (firstAt < 0) firstAt = total; diffs++; }
            stretchedBlocks += hifi_stretching(&h);
            if (hifi_pos_q16(&h) != ref.status().posQ16) { if (firstAt < 0) firstAt = total; diffs++; }
        }
    };
    auto tempo = [&](float permille) {                     // the C6 fader at range 8 % for permille
        const float position = 0.5f + permille / (2.0f * 8 * 10.0f);
        ref.post(ev(deck::Type::TempoFader, true, 0, position));
        const int16_t pm = (int16_t)((position - 0.5f) * 2.0f * 8 * 10.0f);
        hifi_set_speed(&h, HIFI_ONE + pm * HIFI_ONE / 1000);
    };
    run(2);                                                 // loaded, stopped: silence
    ref.post(ev(deck::Type::PlayPause)); hifi_play(&h, 1); run(30);   // play (a fade in)
    ref.post(ev(deck::Type::KeyLock)); hifi_set_keylock(&h, 1); run(10);   // keylock at 1x: straight still
    tempo(80); run(60);                                     // +8 %: the keylock takes over (crossfaded)
    tempo(-80); run(60);                                    // -8 %
    tempo(0); run(20);                                      // unity: back to straight
    tempo(5); run(20);                                      // +0.5 %: past the 0.1 % band, keylocked
    tempo(60); run(20);
    ref.post(ev(deck::Type::KeyLock)); hifi_set_keylock(&h, 0); run(30);   // keylock off at +6 %: varispeed
    ref.post(ev(deck::Type::KeyLock)); hifi_set_keylock(&h, 1); run(10);
    // CUE while playing: back to the cue (0), paused (the C6 deck does both; the app does them on hifi)
    ref.post(ev(deck::Type::Cue)); hifi_play(&h, 0); hifi_seek(&h, 0); run(4);
    // hold CUE: preview from the cue; release: back to it
    ref.post(ev(deck::Type::Cue)); hifi_play(&h, 1); run(20);
    ref.post(ev(deck::Type::Cue, false)); hifi_play(&h, 0); hifi_seek(&h, 0); run(4);
    ref.post(ev(deck::Type::PlayPause)); hifi_play(&h, 1); run(40);
    ref.post(ev(deck::Type::PlayPause)); hifi_play(&h, 0); run(5);   // stop: a fade out, then silence
    CHECK(diffs == 0, "%u Hz %s: %ld of %ld samples (and positions) differ from the C6 engine, the first at %ld",
          rate, ch == 2 ? "stereo" : "mono", diffs, total, firstAt);
    CHECK(stretchedBlocks > 150, "the keylock engine played (%d blocks)", stretchedBlocks);
}

static void the_window_is_the_c6_engines(void)
{
    for (int hop : {256, 512}) {
        nano::StretchWindow w;
        w.build(hop);
        static hifi_deck h;
        const hifi_pcm p = {0, 0, hop == 512 ? 44100u : 22050u, 2};
        hifi_init(&h, &p);
        int bad = 0;
        for (int i = 0; i < hop; i++) bad += w.rise(i) != h.win[i];
        CHECK(h.hop == hop && bad == 0, "the sin^2 window (hop %d) as sinf's: %d entries differ", hop, bad);
    }
}

static void backwards_and_gain(void)
{
    auto pcm = track(44100, 2, 2);
    static hifi_deck h;
    const hifi_pcm p = {pcm.data(), 88200, 44100, 2};
    hifi_init(&h, &p);
    hifi_seek(&h, 44100);
    hifi_set_speed(&h, -HIFI_ONE);
    hifi_set_keylock(&h, 1);                               // (backwards: varispeed, keylock or not)
    hifi_play(&h, 1);
    int16_t o[2 * HIFI_BLOCK];
    for (int i = 0; i < 20; i++) hifi_render(&h, o, HIFI_BLOCK);
    CHECK(hifi_pos(&h) == 44100 - 20 * HIFI_BLOCK && !hifi_stretching(&h), "backwards at 1x: pos %d", hifi_pos(&h));
    // reversed exactly: the samples are the track's, last first
    CHECK(o[2 * 10] == pcm[2 * (44100 - 19 * HIFI_BLOCK - 10)], "backwards plays the track reversed");
    hifi_set_speed(&h, HIFI_ONE);
    hifi_render(&h, o, HIFI_BLOCK);                        // (the direction change isn't a fade: same voice)
    hifi_set_gain(&h, 16384);
    hifi_render(&h, o, HIFI_BLOCK);
    const int32_t at = hifi_pos(&h) - HIFI_BLOCK;
    CHECK(o[0] == (int16_t)(pcm[2 * at] / 2) || o[0] == (int16_t)((pcm[2 * at] - 1) / 2) || o[0] == (int16_t)(pcm[2 * at] >> 1),
          "half gain halves (%d vs %d)", o[0], pcm[2 * at]);
    // an empty deck renders silence
    static hifi_deck e;
    const hifi_pcm none = {0, 0, 44100, 2};
    hifi_init(&e, &none);
    hifi_play(&e, 1);
    hifi_render(&e, o, HIFI_BLOCK);
    int loud = 0;
    for (int i = 0; i < 2 * HIFI_BLOCK; i++) loud |= o[i];
    CHECK(!loud, "no track: silence");
}

int main(void)
{
    the_window_is_the_c6_engines();
    same_as_the_c6_engine(44100, 2);
    same_as_the_c6_engine(22050, 1);
    same_as_the_c6_engine(48000, 2);
    backwards_and_gain();
    printf("test_hifi: %d checks, %d failed\n", s_n, s_fail);
    return s_fail != 0;
}
