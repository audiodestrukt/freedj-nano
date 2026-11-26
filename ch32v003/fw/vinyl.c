// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// vinyl: see vinyl.h (freedj-esp's src/vinyl_rules.h in C).
#include "vinyl.h"

static void zero(void* p, uint32_t n) { uint8_t* b = p; while (n--) *b++ = 0; }   // (freestanding: no libc)

static float absf(float x) { return x < 0 ? -x : x; }

void vinyl_win_init(vinyl_win* w) { zero(w, sizeof *w); }

const vinyl_rec* vinyl_step(vinyl_win* w, uint32_t nowUs, int16_t peak, tc_dec* dec, int absolute)
{
    vinyl_rec* r = &w->r;
    r->landed = r->placed = r->gaveUp = r->armedNow = 0;
    const float sp = dec->speed, m = absf(sp);
    const int carrier = peak >= VINYL_MIN_PEAK && m >= VINYL_MIN_SPEED && m <= VINYL_MAX_SPEED;
    if (w->haveCarrier && nowUs - w->lastCarrierUs > 10000000u) w->haveCarrier = 0;   // long gone (and wrap-safe)
    const uint32_t gapUs = w->haveCarrier ? nowUs - w->lastCarrierUs : 0xFFFFFFFFu;
    if (carrier) { w->lastCarrierUs = nowUs; w->haveCarrier = 1; }
    const uint8_t was = r->signal;
    r->signal = w->haveCarrier && nowUs - w->lastCarrierUs < VINYL_DROP_MS * 1000u;
    if (r->signal && !was && gapUs >= VINYL_LIFT_MS * 1000u && absolute) {
        r->settling = 1;
        r->landed = 1;
        w->landedUs = nowUs;
        tc_relock(dec);
    }
    if (r->settling) {
        r->settleMs = (int32_t)((nowUs - w->landedUs) / 1000u);
        if (dec->locked && dec->position >= 0) {
            r->settling = 0;
            r->placed = 1;
            r->drops++;
        } else if (r->settleMs >= VINYL_SETTLE_MAX_MS || !r->signal) {
            r->settling = 0;
            r->gaveUp = 1;
        }
    }
    if (carrier && m > 0.6f && m < 1.5f) {        // 33 or 45 rpm
        if (!w->steady) { w->steady = 1; w->steadySinceUs = nowUs; }
        if (!r->armed && nowUs - w->steadySinceUs >= VINYL_ARM_MS * 1000u) { r->armed = 1; r->armedNow = 1; }
    } else {
        w->steady = 0;
    }
    r->speed = r->signal && !r->settling ? sp : 0.0f;
    return r;
}

void vinyl_follow_init(vinyl_follow* f)
{
    zero(f, sizeof *f);
    f->sinceSeekMs = 1e9f;
}

vinyl_out vinyl_follow_step(vinyl_follow* f, float raw, float smooth, float dtMs, int hasTarget, float targetMs,
                            float deckMs, uint32_t drops)
{
    vinyl_out o = {0, 0, 0, 0};
    const float m = absf(raw);
    const int steady = m >= 0.5f && m <= 2.0f && absf(raw - smooth) <= 0.03f * m;
    f->steadyMs = steady ? f->steadyMs + dtMs : 0.0f;
    if (f->steadyMs > 1e6f) f->steadyMs = 1e6f;
    if (f->sinceSeekMs < 1e9f) f->sinceSeekMs += dtMs;
    if (!hasTarget) return o;
    const float err = targetMs - deckMs;          // > 0: the deck is behind the needle
    const int dropped = drops != f->dropsSeen;
    f->dropsSeen = drops;
    int jump;
    if (dropped || !f->placed) jump = absf(err) >= VINYL_DEAD_MS;
    else jump = f->steadyMs >= VINYL_STEADY_MS && absf(err) >= VINYL_SEEK_MS && f->sinceSeekMs >= VINYL_SEEK_GAP_MS;
    o.drop = dropped || !f->placed;
    f->placed = 1;
    if (jump) {
        o.seek = 1;
        o.seekMs = targetMs < 0 ? 0 : targetMs;
        f->sinceSeekMs = 0;
    } else {
        o.drop = 0;
        if (f->steadyMs >= VINYL_STEADY_MS && absf(err) >= VINYL_DEAD_MS) {
            float b = err / VINYL_BEND_TAU_MS;
            if (b > VINYL_BEND_MAX) b = VINYL_BEND_MAX;
            if (b < -VINYL_BEND_MAX) b = -VINYL_BEND_MAX;
            o.bend = b * m;
        }
    }
    return o;
}

void vinyl_scan_begin(vinyl_scan* s, uint32_t rate)
{
    zero(s, sizeof *s);
    s->found = -1;
    for (int i = 0; i < VINYL_SCAN_N; i++) {
        tc_begin(&s->dec[i], rate, i < 8 ? TC_SERATO_VINYL : TC_SERATO_VINYL_B, 0, 0);   // (no table while scanning)
        tc_flags(&s->dec[i], (uint32_t)i & 7u);
    }
}

int vinyl_scan_push(vinyl_scan* s, const int16_t* lr, uint32_t frames, uint32_t nowUs)
{
    if (s->found >= 0) return 1;
    (void)nowUs;
    for (int i = 0; i < VINYL_SCAN_N; i++) {
        tc_dec* d = &s->dec[i];
        tc_push(d, lr, frames);
        // locked forwards, added up: a real record's lock breaks now and then (a bit misread), and the wrong side
        // or flags never lock forwards at all, so the total, not a run, is the evidence
        if (d->locked && d->speed > 0.6f && d->speed < 1.5f) s->goodUs[i] += frames * 1000000u / d->rate;
        if (s->goodUs[i] >= VINYL_SCAN_MS * 1000u) { s->found = (int8_t)i; return 1; }
    }
    return 0;
}

int vinyl_scan_side(const vinyl_scan* s) { return s->found >= 8 ? TC_SERATO_VINYL_B : TC_SERATO_VINYL; }
uint32_t vinyl_scan_flags(const vinyl_scan* s) { return s->found < 0 ? 0 : (uint32_t)s->found & 7u; }
