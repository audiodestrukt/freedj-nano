// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// linksync: see linksync.h (freedj-3000's PeerPhase and SyncPhase in C).
#include "linksync.h"

// (no libm)
static float floorF(float x) { const float t = (float)(int32_t)x; return t > x ? t - 1.0f : t; }
static float roundF(float x) { return floorF(x + 0.5f); }
static float remOne(float x) { return x - floorF(x); }               // rem_euclid(1)
static float clampF(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float absF(float x) { return x < 0 ? -x : x; }

void ls_peer_init(ls_peer* p)
{
    p->k = -1; p->f = 0; p->barOffset = -1; p->foldedUs = 0; p->pending = 0;
}

float ls_peer_phase(const ls_peer* p) { return (float)p->k + p->f; }

static void setBar(ls_peer* p, uint8_t bib, int32_t atArrivalBeat)
{
    if (bib >= 1 && bib <= 4) {
        int32_t o = ((int32_t)bib - 1 - atArrivalBeat) % 4;
        p->barOffset = (int8_t)(o < 0 ? o + 4 : o);
    }
}

float ls_peer_advance(ls_peer* p, uint32_t now, uint32_t beatUs, int haveBeat, float bpm, uint8_t bib, int playing,
                      float dtS, uint8_t* shown)
{
    if (bpm <= 0 || !haveBeat) {
        ls_peer_init(p);
        if (shown) *shown = bib;
        return 0;
    }
    const float period = 60.0f / bpm;
    const float age = (float)(uint32_t)(now - beatUs) / 1e6f;
    const int running = playing || age < 2.0f * period;
    if (p->k < 0) {                               // the first packet: its beat was `age` ago
        const float ph = age / period;
        p->k = (int32_t)floorF(ph);
        p->f = ph - (float)p->k;
        p->foldedUs = beatUs;
        p->pending = 0;
        setBar(p, bib, 0);
    } else if (running) {
        const float inc = dtS / period;
        const float adj = clampF(p->pending, -inc * LS_SLEW, inc * LS_SLEW);
        p->f += inc + adj;
        p->pending -= adj;
        while (p->f >= 1.0f) { p->f -= 1.0f; p->k++; }
        while (p->f < 0.0f && p->k > 0) { p->f += 1.0f; p->k--; }
    }
    if (beatUs != p->foldedUs) {                  // a new packet marked a beat at now - age: pull toward it
        p->foldedUs = beatUs;
        const float x = p->f - age / period;      // (the free run at its arrival, less the whole beats k)
        float err = remOne(x);
        if (err > 0.5f) err -= 1.0f;              // beats, +-0.5
        p->pending -= err * LS_PULL;
        setBar(p, bib, p->k + (int32_t)roundF(x));
    }
    if (shown) *shown = p->barOffset < 0 ? bib : (uint8_t)((p->k + p->barOffset) % 4 + 1);
    return p->f;
}

float ls_phase_err(float frac, float offset)
{
    const float e = frac - offset;
    return e - roundF(e);
}

int ls_sync_tick(ls_sync* s, int sync, int jogging)
{
    if (!sync) { s->pending = 0; return 1; }
    if (jogging) s->pending = 1;
    return 0;
}

int ls_sync_on_master_beat(ls_sync* s, float frac, float offset, int jogging, float* out)
{
    if (jogging) return LS_NONE;
    if (s->pending) {
        s->pending = 0;
        *out = ls_phase_err(frac, 0);
        return LS_HOLD;
    }
    const float err = ls_phase_err(frac, offset);
    // half the error per master beat: bounded like a jog nudge, and halving avoids the overshoot a full
    // correction gives (freedj-3000 measured +-0.02 beat of oscillation at gain 1)
    if (absF(err) > 0.01f) { *out = clampF(-err * 0.5f, -0.03f, 0.03f); return LS_CORRECT; }
    return LS_NONE;
}

float ls_tempo_match(float masterBpm, float trackBpm, float range)
{
    if (masterBpm <= 0 || trackBpm <= 0) return 1.0f;
    return clampF(masterBpm / trackBpm, 1.0f - range, 1.0f + range);
}
