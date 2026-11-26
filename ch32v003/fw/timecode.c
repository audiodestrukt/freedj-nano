// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// timecode: see timecode.h. freedj-esp's src/timecode.cpp in C: floats where it has them (the AC791N's FPU,
// -mcpu=r3 -mfprev1: 47 ns a stereo frame on an FM-1, 2.3 % of a core at 48 kHz, where a Q16 port
// took 79 ns; the lock and silence checks once a push, not a sample. fm1/probes/tcbench.c).
#include "timecode.h"

#define SAMPLE_SHIFT 16
#define ZERO_THRESH  (128 << 16)
#define REF_PEAKS_AVG 48                          // cycles: the bit level's running mean
#define VALID_BITS   24                           // bits right in a row: a lock
#define SPEED_ALPHA  0.15f

typedef struct { uint32_t resolution, bits, seed, taps, length; } tc_def;
// reverse-engineered facts about Serato's media (as in xwax and others): data, not expression
static const tc_def kDefs[] = {
    {1000, 20, 0x59017, 0x361e4, 712000},         // Serato Control Vinyl (2nd edition, side A)
    {1000, 20, 0xd8b40, 0x34d54, 950000},         // Serato Control CD
    {1000, 20, 0x8f3c6, 0x4f0d8, 922000},         // Serato Control Vinyl (2nd edition, side B: side A's taps reversed)
};
static const tc_def* def(int f) { return &kDefs[f >= 0 && f <= TC_SERATO_VINYL_B ? f : 0]; }

static uint32_t parity(uint32_t x)
{
    x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return x & 1u;
}
static uint32_t fwd(uint32_t cur, uint32_t taps, uint32_t bits)   // the new bit in at the top
{
    return (cur >> 1) | (parity(cur & (taps | 1u)) << (bits - 1));
}
static uint32_t rev(uint32_t cur, uint32_t taps, uint32_t bits)   // the new bit in at the bottom
{
    const uint32_t mask = (1u << bits) - 1u;
    return ((cur << 1) & mask) | parity(cur & ((taps >> 1) | (1u << (bits - 1))));
}

uint32_t tc_length(int f) { return def(f)->length; }
uint32_t tc_seed(int f) { return def(f)->seed; }
uint32_t tc_next(int f, uint32_t w) { return fwd(w, def(f)->taps, def(f)->bits); }

void tc_reset(tc_dec* d)
{
    const tc_chan z = {0, 0, 0, 0};
    d->pri = z; d->sec = z;
    d->sincePri = 0;
    d->forwards = 1; d->locked = 0;
    d->speed = 0;
    d->refLevel = 0;
    d->bitDue = 0; d->hcSum = d->hcN = d->hcPeak = 0;
    tc_relock(d);
}
void tc_relock(tc_dec* d)
{
    d->bitstream = d->expected = d->valid = 0;
    d->position = -1;
    d->pending = 0; d->pendBits = 0;
    d->locked = 0;
}
void tc_flags(tc_dec* d, uint32_t flags) { d->flags = flags; tc_reset(d); }
void tc_level(tc_dec* d, int mode) { d->levelMode = (uint8_t)mode; tc_reset(d); }

void tc_begin(tc_dec* d, uint32_t rate, int format, tc_lut lut, void* ctx)
{
    const tc_def* f = def(format);
    d->rate = rate; d->bits = f->bits; d->taps = f->taps; d->seed = f->seed;
    d->length = f->length; d->resolution = f->resolution; d->flags = 0; d->levelMode = TC_LEVEL_SAMPLE;
    const float dt = 1.0f / (float)rate;
    d->zeroAlpha = dt / (0.001f + dt);           // a 1 ms DC track
    d->refPeriod = (float)rate / (float)f->resolution;
    d->lut = lut; d->lutCtx = ctx;
    tc_reset(d);
}

static void chan(tc_chan* c, int32_t v, float alpha)
{
    c->ticker++;
    c->swapped = 0;
    if (v > c->zero + ZERO_THRESH && !c->positive) { c->swapped = 1; c->positive = 1; c->ticker = 0; }
    else if (v < c->zero - ZERO_THRESH && c->positive) { c->swapped = 1; c->positive = 0; c->ticker = 0; }
    c->zero += (int32_t)(alpha * (float)(v - c->zero));
}

static void place(tc_dec* d)                      // the position, from the table, once a lock has none
{
    if (!d->lut) return;
    if (!d->pending) { d->pending = 1; d->pendWindow = d->bitstream; d->pendBits = 0; }
    const int32_t p = d->lut(d->pendWindow, d->lutCtx);
    if (p == -2) return;                          // (asked: the answer comes later)
    d->pending = 0;
    if (p < 0) return;
    // The table names a window by its newest bit going forward; going backwards the newest bit is the window's
    // oldest, bits - 1 cycles earlier (found on freedj-3000's DVS). Then the bits since it was asked.
    int32_t q = d->forwards ? p : (p >= (int32_t)(d->bits - 1) ? p - (int32_t)(d->bits - 1) : 0);
    q += d->pendBits;
    d->position = q < 0 ? 0 : q;
}

static void bit(tc_dec* d, int32_t m)
{
    if (d->refLevel == 0) d->refLevel = 0x3FFFFFFF;   // high: the first bits read 0 as it walks down
    const uint32_t b = m > d->refLevel ? 1u : 0u;
    if (d->forwards) {
        d->expected = fwd(d->expected, d->taps, d->bits);
        d->bitstream = (d->bitstream >> 1) | (b << (d->bits - 1));
    } else {
        d->expected = rev(d->expected, d->taps, d->bits);
        d->bitstream = ((d->bitstream << 1) & ((1u << d->bits) - 1u)) | b;
    }
    // (all zeros is the LFSR's fixed point, not a state on any record: a constant level - the wrong channel as
    // primary - would "predict" itself for ever)
    if (d->expected == d->bitstream && d->bitstream) d->valid++;
    else { d->expected = d->bitstream; d->valid = 0; d->position = -1; d->pending = 0; }   // re-sync
#ifdef TC_TRACE
    TC_TRACE(d, m, b, d->valid != 0);             // (a probe's look at each bit: its level, the call, right or not)
#endif
    d->refLevel -= d->refLevel / REF_PEAKS_AVG;
    d->refLevel += m / REF_PEAKS_AVG;
    if (d->valid > VALID_BITS) {
        if (d->position >= 0) d->position += d->forwards ? 1 : -1;   // placed: counted on
        else {
            if (d->pending) d->pendBits += d->forwards ? 1 : -1;
            place(d);
        }
        if (d->position < 0 && d->position != -1) d->position = 0;
    }
}

static void sample(tc_dec* d, int32_t p, int32_t s)
{
    chan(&d->pri, p, d->zeroAlpha);
    chan(&d->sec, s, d->zeroAlpha);
    if (d->pri.swapped || d->sec.swapped) {       // direction: primary crossed -> forwards if they differ
        uint8_t f = d->pri.swapped ? d->pri.positive != d->sec.positive : d->pri.positive == d->sec.positive;
        if (d->flags & TC_SWITCH_PHASE) f = !f;
        if (f != d->forwards) { d->forwards = f; d->valid = 0; d->position = -1; d->pending = 0; }
    }
    d->sincePri++;
    if (d->pri.swapped) {                         // speed: the half period against the nominal
        const uint32_t half = d->sincePri;
        d->sincePri = 0;
        if (half) {
            float r = d->refPeriod / (2.0f * (float)half);
            if (!d->forwards) r = -r;
            d->speed = (1.0f - SPEED_ALPHA) * d->speed + SPEED_ALPHA * r;
        }
    }
    const uint8_t pol = !(d->flags & TC_SWITCH_POLARITY);
    int32_t m = p / 2 - d->pri.zero / 2;
    if (m < 0) m = -m;
    if (d->pri.swapped) {                         // a primary half-cycle ended: a bit due in it is called now
        if (d->bitDue) {
            d->bitDue = 0;
            const uint32_t mean = d->hcN ? d->hcSum / d->hcN << 8 : 0;
#ifdef TC_TRACE3
            TC_TRACE3(d, d->dueSample, (int32_t)mean, (int32_t)d->hcPeak);
#endif
            if (d->levelMode == TC_LEVEL_MEAN) bit(d, (int32_t)mean);
            else if (d->levelMode == TC_LEVEL_PEAK) bit(d, (int32_t)d->hcPeak);
        }
        d->hcSum = 0; d->hcN = 0; d->hcPeak = 0;
    }
    d->hcSum += (uint32_t)m >> 8; d->hcN++;       // (>> 8: a half-cycle's sum fits 32 bits)
    if ((uint32_t)m > d->hcPeak) d->hcPeak = (uint32_t)m;
    if (d->sec.swapped && d->pri.positive == pol) {
        d->bitDue = 1; d->dueSample = m;
        if (d->levelMode == TC_LEVEL_SAMPLE) bit(d, m);
    }
}
static void status(tc_dec* d)                     // (once a push, not a sample: it's what's read out)
{
    const uint32_t quiet = d->rate / 10u;         // no crossings for 100 ms: stopped
    if (d->pri.ticker > quiet && d->sec.ticker > quiet) { d->speed = 0; d->locked = 0; return; }
    const float a = d->speed < 0 ? -d->speed : d->speed;
    d->locked = d->valid > VALID_BITS && a > 0.05f && a < 4.0f;
}

void tc_push(tc_dec* d, const int16_t* lr, uint32_t frames)
{
    const int swap = (d->flags & TC_SWITCH_PRIMARY) != 0;   // (default: primary R, secondary L)
    for (uint32_t i = 0; i < frames; i++) {
        const int32_t l = (int32_t)lr[2 * i] << SAMPLE_SHIFT, r = (int32_t)lr[2 * i + 1] << SAMPLE_SHIFT;
        if (swap) sample(d, l, r); else sample(d, r, l);
    }
    status(d);
}
