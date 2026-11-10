// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for nano-lite (the CH32V003 engine), against the C6 engine where they overlap.
#include "lite.h"
#include "nano/stretch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint32_t kRate = 22050;

struct Mem { std::vector<uint8_t> u; };
static void memRead(void* ctx, uint32_t off, uint8_t* dst, uint8_t n) { memcpy(dst, ((Mem*)ctx)->u.data() + off, n); }

static std::vector<int16_t> tone(float hz, float secs, float amp = 12000)
{
    std::vector<int16_t> v((size_t)(kRate * secs));
    for (size_t i = 0; i < v.size(); i++) v[i] = (int16_t)(amp * sinf(6.2831853f * hz * i / kRate));
    return v;
}

// A kick every 0.5 s (decaying 55 Hz) over a chord.
static std::vector<int16_t> music(float secs)
{
    std::vector<int16_t> v((size_t)(kRate * secs));
    for (size_t i = 0; i < v.size(); i++) {
        const float t = (float)i / kRate, tk = fmodf(t, 0.5f);
        float s = 9000 * expf(-tk * 9) * sinf(6.2831853f * 55 * tk);
        for (float f : {220.f, 277.2f, 329.6f}) s += 2500 * sinf(6.2831853f * f * t);
        v[i] = (int16_t)s;
    }
    return v;
}

struct Lite {
    Mem m; lite_src src{}; lite_deck d{};
    explicit Lite(const std::vector<int16_t>& pcm) {
        for (int16_t s : pcm) m.u.push_back(lite_ulaw_encode(s));
        src.read = memRead; src.ctx = &m; src.frames = (uint32_t)pcm.size();
        lite_init(&d, &src);
    }
    std::vector<int16_t> run(int n) {
        std::vector<int16_t> o(n);
        for (int k = 0; k < n; k += 64) lite_render(&d, &o[k], std::min(64, n - k));
        return o;
    }
};

static double freqOf(const int16_t* x, int n)
{
    int z = 0;
    for (int i = 1; i < n; i++) if ((x[i - 1] < 0) != (x[i] < 0)) z++;
    return z * 0.5 * kRate / n;
}
static double rms(const int16_t* x, int n) { double s = 0; for (int i = 0; i < n; i++) s += (double)x[i] * x[i]; return sqrt(s / n); }
static int maxStep(const int16_t* x, int n) { int m = 0; for (int i = 1; i < n; i++) m = std::max(m, abs(x[i] - x[i - 1])); return m; }

static void writeWav(const char* path, const std::vector<int16_t>& x)
{
    FILE* f = fopen(path, "wb"); if (!f) return;
    const uint32_t data = (uint32_t)x.size() * 2, riff = 36 + data, fmt = 16, rate = kRate, br = kRate * 2;
    const uint16_t pcm = 1, ch = 1, ba = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(x.data(), 2, x.size(), f); fclose(f);
}

static void testUlaw()
{
    int worst = 0;
    for (int s = -32768; s < 32768; s += 7) {
        const int e = abs(lite_ulaw[lite_ulaw_encode((int16_t)s)] - s), tol = abs(s) / 16 + 8;
        if (e > tol) worst++;
    }
    CHECK(worst == 0, "mu-law round trip: %d samples off by more than 1/16", worst);
}

static void testPitch()
{
    for (float sp : {0.92f, 1.08f, 1.16f})
        for (float f : {50.f, 80.f, 120.f, 440.f, 1000.f}) {
            Lite L(tone(f, 8));
            lite_set_keylock(&L.d, 1); lite_set_speed(&L.d, (int32_t)lrintf(sp * 65536)); lite_play(&L.d, 1);
            auto o = L.run(kRate * 4);
            const double got = freqOf(&o[kRate / 2], (int)o.size() - kRate / 2);
            CHECK(fabs(got / f - 1) < 0.01, "keylock %.2f: %.0f Hz plays at %.1f Hz", sp, f, got);
            CHECK(L.d.stretch, "keylock engine on at %.2f", sp);
            CHECK(L.d.misses == 0, "keylock %.2f %.0f Hz: %u ring misses", sp, f, L.d.misses);
        }
    Lite V(tone(440, 6));
    lite_set_speed(&V.d, 0x20000); lite_play(&V.d, 1);
    auto o = V.run(kRate);
    const double got = freqOf(&o[2048], (int)o.size() - 2048);
    CHECK(fabs(got / 880 - 1) < 0.01, "varispeed 2x: 440 Hz plays at %.1f Hz", got);
}

static void testSpeedAndRings()
{
    auto m = music(30);
    for (int kl = 0; kl < 2; kl++)
        for (float sp : {0.5f, 0.75f, 0.92f, 1.0f, 1.08f, 1.25f, 1.5f, 2.0f}) {
            Lite L(m);
            lite_set_keylock(&L.d, kl); lite_set_speed(&L.d, (int32_t)lrintf(sp * 65536)); lite_play(&L.d, 1);
            L.run(1024);
            const int32_t p0 = lite_pos(&L.d);
            const int n = kRate * 5;
            L.run(n);
            const double moved = lite_pos(&L.d) - p0;
            CHECK(fabs(moved - n * sp) < 300, "%s %.2f: moved %.0f frames, want %.0f", kl ? "keylock" : "varispeed", sp, moved, n * sp);
            CHECK(L.d.misses == 0, "%s %.2f: %u ring misses", kl ? "keylock" : "varispeed", sp, L.d.misses);
        }
    // jumps, engine switches and tempo moves while playing
    Lite L(m);
    lite_play(&L.d, 1);
    srand(7);
    int worstStep = 0;
    for (int r = 0; r < 200; r++) {
        switch (rand() % 4) {
        case 0: lite_seek(&L.d, rand() % (int)(m.size() - kRate)); break;
        case 1: lite_set_keylock(&L.d, rand() & 1); break;
        default: lite_set_speed(&L.d, 0x10000 + (rand() % 20000) - 10000); break;
        }
        auto o = L.run(2048);
        worstStep = std::max(worstStep, maxStep(o.data(), (int)o.size()));
    }
    CHECK(L.d.misses == 0, "seeks and switches: %u ring misses", L.d.misses);
    const int ownStep = maxStep(m.data(), (int)m.size());
    CHECK(worstStep < ownStep * 2, "seeks and switches: max step %d vs the music's own %d (clicks?)", worstStep, ownStep);
}

// lite_pos() never goes backwards while playing forward (the display, cue points and the
// end of the track all read it). It did at every hop boundary in keylock, by one hop.
static void testPosMonotonic()
{
    auto m = music(10);
    for (int kl = 0; kl < 2; kl++)
        for (float sp : {0.5f, 0.92f, 1.0f, 1.035f, 1.5f}) {
            Lite L(m);
            lite_set_keylock(&L.d, kl); lite_set_speed(&L.d, (int32_t)lrintf(sp * 65536)); lite_play(&L.d, 1);
            int32_t last = -1, back = 0;
            int16_t o[32];
            for (int b = 0; b < 2000; b++) {
                lite_render(&L.d, o, 32);
                const int32_t p = lite_pos(&L.d);
                if (p < last) back++;
                last = p;
            }
            CHECK(back == 0, "%s %.3f: position went backwards %d times", kl ? "keylock" : "varispeed", sp, back);
        }
}

static void testQuality()
{
    auto m = music(10);
    // straight playback: mu-law only
    Lite S(m);
    lite_play(&S.d, 1);
    auto o = S.run(kRate * 5);
    double err = 0, sig = 0;
    for (int i = 2048; i < (int)o.size(); i++) { const double e = o[i] - m[i]; err += e * e; sig += (double)m[i] * m[i]; }
    const double snr = 10 * log10(sig / err);
    CHECK(snr > 30, "mu-law playback SNR %.1f dB", snr);
    printf("  mu-law playback: SNR %.1f dB\n", snr);

    // keylock at +8 %: lite (mu-law, linear fade) against the C6 engine (16-bit, sin^2)
    Lite L(m);
    lite_set_keylock(&L.d, 1); lite_set_speed(&L.d, 0x10000 + 0x10000 * 8 / 100); lite_play(&L.d, 1);
    auto lo = L.run(kRate * 8);
    nano::Pcm pcm{m.data(), (uint32_t)m.size(), kRate, 1};
    nano::StretchWindow w; w.build(nano::stretchHop(kRate));
    nano::Stretch st; st.begin(&w); st.reset(0);
    std::vector<int32_t> no32(kRate * 8 * 2);
    const int nn = (int)kRate * 8;
    for (int k = 0; k < nn; k += 256) st.render(pcm, nano::kOne + nano::kOne * 8 / 100, &no32[2 * k], std::min(256, nn - k));
    std::vector<int16_t> no(kRate * 8);
    for (size_t i = 0; i < no.size(); i++) no[i] = nano::sat16(no32[2 * i]);
    const double rl = rms(&lo[kRate], kRate * 6), rn = rms(&no[kRate], kRate * 6), ri = rms(m.data(), (int)m.size());
    CHECK(fabs(rl / rn - 1) < 0.1, "keylock level: lite %.0f, C6 %.0f", rl, rn);
    printf("  keylock +8 %%: level lite %.0f, C6 %.0f, source %.0f\n", rl, rn, ri);
    writeWav("out/lite_source.wav", m);
    writeWav("out/lite_keylock_108.wav", lo);
    writeWav("out/c6_keylock_108.wav", no);
}

int main()
{
    printf("lite_deck: %u bytes of RAM\n", (unsigned)sizeof(lite_deck));
    testUlaw();
    testPitch();
    testSpeedAndRings();
    testPosMonotonic();
    testQuality();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
