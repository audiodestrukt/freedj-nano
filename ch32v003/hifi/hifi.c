// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// nano-hifi: see hifi.h. components/nano/src/stretch.cpp and deck.cpp's render, in C.
#include "hifi.h"

#define STRETCH_MIN (HIFI_ONE / 2)
#define STRETCH_MAX (HIFI_ONE * 2)
#define UNITY_BAND  64                            // within ~0.1 % of 1.0: play straight

static int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

// ── the track ──
static void frame(const hifi_pcm* p, int32_t f, int32_t* l, int32_t* r)
{
    if (f < 0 || (uint32_t)f >= p->frames) { *l = *r = 0; return; }
    const int16_t* s = p->data + (uint32_t)f * p->channels;
    *l = s[0];
    *r = p->channels > 1 ? s[1] : s[0];
}
static int32_t mono(const hifi_pcm* p, int32_t f) { int32_t l, r; frame(p, f, &l, &r); return (l + r) >> 1; }
static int valid(const hifi_pcm* p) { return p->data && p->frames && p->rate; }

// ── the window: sin^2 over the rising half of a 2 x hop frame (fall = 32768 - rise: unity gain) ──
// (no libm: sin by its series on 0..pi/2, single precision, as sinf to the last bit for these points;
// the host test checks the table against the C++ engine's)
static float sinQ(float x)
{
    const float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}
static void buildWindow(hifi_deck* d, uint16_t hop)
{
    d->hop = hop;
    for (int i = 0; i < hop; i++) {
        const float s = sinQ(3.14159265f * 0.5f * (float)i / (float)hop);
        const float v = s * s * 32768.0f;
        int32_t r = (int32_t)(v + 0.5f);          // to nearest, a half to even, as lrintf: i = 199 of 256
        if ((float)r - v == 0.5f && (r & 1)) r--; // (398 of 512) is 28920.5 exactly, and sinf's table has 28920
        d->win[i] = (uint16_t)r;
    }
}

// ── keylock: WSOLA (stretch.cpp) ──
static void stretchReset(hifi_stretch* st, uint16_t hop, int32_t f)
{
    // the "previous" frame's second half starts at f, so the first hop fades that straight audio out while the
    // first matched frame fades in
    st->cur = f - hop;
    st->prev = st->cur;
    st->anaQ16 = (int64_t)st->cur << 16;
    st->i = 0;
    st->fresh = 1;
    st->lastSpeed = HIFI_ONE;
}
static int64_t stretchPos(const hifi_stretch* st, uint16_t hop)
{
    return st->fresh ? (int64_t)(st->cur + hop) << 16 : st->anaQ16 + (int64_t)st->i * st->lastSpeed;
}

// Search +-1 hop (11.6 ms: bass needs up to half its period to line up). Coarse: every cs-th offset over a
// whole hop sampled every cs-th; fine: every offset within one coarse step, hop/2 every 2nd.
static int32_t find(const hifi_pcm* p, uint16_t hop, int32_t nominal, int32_t target)
{
    const int search = hop;
    const int cs = hop / 64 < 2 ? 2 : hop / 64;
    const int cpts = hop / cs, fpts = hop / 4;
    int32_t cref[64], fref[HIFI_MAX_HOP / 4];
    for (int k = 0; k < cpts; k++) cref[k] = mono(p, target + cs * k);
    for (int k = 0; k < fpts; k++) fref[k] = mono(p, target + 2 * k);
    int32_t best = nominal;
    uint32_t bestS = 0xFFFFFFFFu;
    for (int32_t dd = -search; dd <= search; dd += cs) {
        uint32_t s = 0;
        for (int k = 0; k < cpts && s < bestS; k++) s += (uint32_t)iabs(mono(p, nominal + dd + cs * k) - cref[k]);
        if (s < bestS) { bestS = s; best = nominal + dd; }
    }
    const int32_t c0 = best;
    bestS = 0xFFFFFFFFu;
    for (int32_t dd = 1 - cs; dd < cs; dd++) {
        uint32_t s = 0;
        for (int k = 0; k < fpts && s < bestS; k++) s += (uint32_t)iabs(mono(p, c0 + dd + 2 * k) - fref[k]);
        if (s < bestS) { bestS = s; best = c0 + dd; }
    }
    return best;
}

static void stretchRender(hifi_deck* d, hifi_stretch* st, int32_t speed, int32_t* out, int n)
{
    const uint16_t hop = d->hop;
    for (int k = 0; k < n; k++) {
        if (st->i == 0) {                          // the next hop
            st->prev = st->cur;
            st->anaQ16 += (int64_t)hop * speed;
            st->lastSpeed = speed;
            st->fresh = 0;
            st->cur = find(&d->pcm, hop, (int32_t)(st->anaQ16 >> 16), st->prev + hop);
        }
        const int32_t rise = d->win[st->i], fall = 32768 - rise;
        int32_t al, ar, bl, br;
        frame(&d->pcm, st->prev + hop + st->i, &al, &ar);
        frame(&d->pcm, st->cur + st->i, &bl, &br);
        out[2 * k] = (al * fall + bl * rise) >> 15;
        out[2 * k + 1] = (ar * fall + br * rise) >> 15;
        if (++st->i == hop) st->i = 0;
    }
}

// ── varispeed: linear interpolation at a Q16 position (any speed, reverse, a scratch's crawl) ──
static void varispeed(const hifi_pcm* p, int64_t* posQ16, int32_t speed, int32_t* out, int n)
{
    for (int k = 0; k < n; k++) {
        const int32_t f = (int32_t)(*posQ16 >> 16);
        const int32_t frac = (int32_t)(*posQ16 & 0xFFFF);
        int32_t al, ar, bl, br;
        frame(p, f, &al, &ar);
        frame(p, f + 1, &bl, &br);
        out[2 * k] = al + (int32_t)(((int64_t)(bl - al) * frac) >> 16);
        out[2 * k + 1] = ar + (int32_t)(((int64_t)(br - ar) * frac) >> 16);
        *posQ16 += speed;
    }
}

static int64_t voicePos(const hifi_deck* d, const hifi_voice* v) { return v->stretch ? stretchPos(&v->st, d->hop) : v->posQ16; }

static void voiceRender(hifi_deck* d, hifi_voice* v, int32_t* out, int n)
{
    if (!v->speed || !valid(&d->pcm)) { for (int i = 0; i < 2 * n; i++) out[i] = 0; return; }
    if (v->stretch) stretchRender(d, &v->st, v->speed, out, n);
    else varispeed(&d->pcm, &v->posQ16, v->speed, out, n);
}

// ── the deck ──
void hifi_init(hifi_deck* d, const hifi_pcm* pcm)
{
    uint8_t* z = (uint8_t*)d;
    for (uint32_t i = 0; i < sizeof *d; i++) z[i] = 0;
    d->pcm = *pcm;
    buildWindow(d, pcm->rate > 32000 ? 512 : 256);
    d->speed = HIFI_ONE;
    d->gainQ15 = 32768;
    d->jump = 1;
}

void hifi_play(hifi_deck* d, int on) { d->playing = on != 0; }
void hifi_set_speed(hifi_deck* d, int32_t speedQ16) { d->speed = speedQ16; }
void hifi_set_keylock(hifi_deck* d, int on) { d->keylock = on != 0; }
void hifi_seek(hifi_deck* d, int32_t f) { d->seekPending = 1; d->seekTo = f; }
void hifi_set_gain(hifi_deck* d, int32_t gainQ15) { d->gainQ15 = gainQ15; }
int64_t hifi_pos_q16(const hifi_deck* d)
{
    if (d->seekPending) return (int64_t)d->seekTo << 16;
    const int64_t p = voicePos(d, &d->v);
    return p < 0 ? 0 : p;
}
int32_t hifi_pos(const hifi_deck* d) { return (int32_t)(hifi_pos_q16(d) >> 16); }
int     hifi_stretching(const hifi_deck* d) { return d->v.stretch; }

void hifi_render(hifi_deck* d, int16_t* out, int n)
{
    if (n > HIFI_BLOCK) n = HIFI_BLOCK;
    hifi_voice old = d->v;                        // (what was playing: faded out under the new if anything jumps)
    if (d->seekPending) {                         // a seek: the new voice there
        d->seekPending = 0;
        if (d->v.stretch) stretchReset(&d->v.st, d->hop, d->seekTo);
        else d->v.posQ16 = (int64_t)d->seekTo << 16;
        d->jump = 1;
    }
    const int32_t speed = d->playing ? d->speed : 0;
    const int32_t dev = speed > HIFI_ONE ? speed - HIFI_ONE : HIFI_ONE - speed;
    const uint8_t want = d->keylock && speed >= STRETCH_MIN && speed <= STRETCH_MAX && dev > UNITY_BAND;
    if (want != d->v.stretch && valid(&d->pcm)) { // the engine switches where the old one is
        const int64_t p = voicePos(d, &d->v);
        d->v.stretch = want;
        if (want) stretchReset(&d->v.st, d->hop, (int32_t)(p >> 16)); else d->v.posQ16 = p;
        d->jump = 1;
    }
    if ((old.speed == 0) != (speed == 0)) d->jump = 1;   // start or stop: fade
    d->v.speed = speed;
    if (d->jump) {
        voiceRender(d, &old, d->a, n);
        voiceRender(d, &d->v, d->b, n);
        for (int k = 0; k < n; k++) {
            const int32_t w = (k << 15) / n;
            for (int c = 0; c < 2; c++) d->a[2 * k + c] = (d->a[2 * k + c] * (32768 - w) + d->b[2 * k + c] * w) >> 15;
        }
        d->jump = 0;
    } else {
        voiceRender(d, &d->v, d->a, n);
    }
    for (int i = 0; i < 2 * n; i++) out[i] = sat16((int32_t)(((int64_t)d->a[i] * d->gainQ15) >> 15));
}
