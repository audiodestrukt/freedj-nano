// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lite.h"

#define RMASK  (LITE_RING - 1)
#define UNITY_BAND 64                 // within ~0.1 % of 1.0: play straight

// a * b for an 8-bit b, by shifts and adds: RV32EC has no multiply instruction,
// and libgcc's __mulsi3 loops over all 32 bits.
static inline __attribute__((always_inline)) int32_t mul8(int32_t a, uint8_t b)
{
    int32_t r = 0;
    if (b & 0x01) r += a;
    if (b & 0x02) r += a << 1;
    if (b & 0x04) r += a << 2;
    if (b & 0x08) r += a << 3;
    if (b & 0x10) r += a << 4;
    if (b & 0x20) r += a << 5;
    if (b & 0x40) r += a << 6;
    if (b & 0x80) r += a << 7;
    return r;
}

// ---- rings ----

// The full-rate ring is 768 bytes, not a power of 2: index it from `rbase`, a multiple of
// 768 that trails the head by less than two ring lengths (no division needed).
static inline __attribute__((always_inline)) uint16_t ridx(const lite_deck* d, int32_t p)
{
    int32_t k = p - d->rbase;
    if (k >= LITE_RING) k -= LITE_RING;
    return (uint16_t)k;
}

static void fetch(lite_deck* d, int32_t upto)
{
    if (upto > (int32_t)d->src.frames) upto = (int32_t)d->src.frames;
    while (d->head < upto) {
        const int32_t h = d->head;
        if (h - d->rbase >= 2 * LITE_RING) d->rbase += LITE_RING;
        uint8_t n = 32 - (uint8_t)(h & 31);                // chunks of <= 32, never across the ring's end
        uint8_t* dst = &d->ring[ridx(d, h)];
        if (h + n > (int32_t)d->src.frames) {
            n = (uint8_t)(d->src.frames - h);
            for (uint8_t k = n; k < 32 - (h & 31); k++) dst[k] = 0xFF;   // mu-law silence past the end
        }
        d->src.read(d->src.ctx, (uint32_t)h, dst, n);
        const int32_t end = h + 32 - (h & 31);
        for (int32_t j = h; j < end; j += 4) {             // decimate 4:1 for the coarse search
            const uint8_t* q = &d->ring[ridx(d, j)];
            d->cring[ridx(d, j) >> 2] =
                (int8_t)((lite_ulaw[q[0]] + lite_ulaw[q[1]] + lite_ulaw[q[2]] + lite_ulaw[q[3]]) >> 10);
        }
        d->head = end;
    }
}

static inline __attribute__((always_inline)) int16_t sample(lite_deck* d, int32_t p)
{
    if (p < 0 || p >= (int32_t)d->src.frames) return 0;
    if (p >= d->head) fetch(d, p + 1);
    if (p < d->base || p < d->head - LITE_RING) { d->misses++; return 0; }
    return lite_ulaw[d->ring[ridx(d, p)]];
}

static inline int8_t coarse(lite_deck* d, int32_t j)
{
    const int32_t p = j << 2;
    if (p < 0 || p + 3 >= (int32_t)d->src.frames) return 0;
    if (p + 3 >= d->head) fetch(d, p + 4);
    if (p < d->base || p < d->head - LITE_RING) { d->misses++; return 0; }
    return d->cring[ridx(d, p) >> 2];
}

static void jumpTo(lite_deck* d, int32_t frame)
{
    if (frame < 0) frame = 0;
    // Start fetching a hop early: the keylock's first frame and search reach back that far.
    int32_t b = (frame - LITE_HOP) & ~31;
    if (b < 0) b = 0;
    d->head = d->base = d->rbase = b;
    d->pos = frame; d->posFrac = 0;
    d->cur = d->prev = frame - LITE_HOP;
    d->ana = d->cur; d->anaFrac = 0;
    d->i = 0; d->fresh = 1;
}

// ---- keylock ----

int32_t lite_find(lite_deck* d, int32_t nominal, int32_t target)
{
    // Candidates: nominal +-1 hop. All the search's reads, and the coming hop's (target
    // onwards and the chosen frame), must fit in the ring at once; clamp the side away
    // from the target if they wouldn't (after a far-off previous choice, or at high speed).
    const int32_t span = 4 * LITE_CPTS, room = LITE_RING - 36;
    int32_t lo = nominal - LITE_HOP, hi = nominal + LITE_HOP;
    if (target <= lo - 7) { if (hi > target + room - span) hi = target + room - span; }
    else if (target >= hi) { if (lo < target + span + 7 - room) lo = target + span + 7 - room; }
    if (hi < lo) { if (target < nominal) lo = hi; else hi = lo; }
    // and never below what the ring still holds (playback may have fetched well ahead)
    if (lo < d->head - LITE_RING + 12) { lo = d->head - LITE_RING + 12; if (hi < lo) hi = lo; }

    // Everything the search reads, fetched once up front. Inside a valid window the loops
    // index the rings directly; the checked reads are only for the edges (track ends,
    // just after a seek).
    const int32_t lowRead = (target < lo - 7 ? target : lo - 7) & ~3;
    const int32_t highRead = (target > hi ? target : hi) + span + 4;
    fetch(d, highRead);
    const int fast = lowRead >= 0 && lowRead >= d->base && lowRead >= d->head - LITE_RING
                     && highRead <= d->head;

    // coarse: every 8th offset (every 2nd of the decimated ring), comparing hop/2 of it.
    // Centre out, so equally good matches (every period of a tone) resolve to the nearest.
    int8_t cref[LITE_CPTS];
    const int32_t tj = target >> 2, nj = nominal >> 2, jlo = lo >> 2, jhi = hi >> 2;
    for (int k = 0; k < LITE_CPTS; k++) cref[k] = coarse(d, tj + k);
    int32_t bestJ = nj < jlo ? jlo : nj > jhi ? jhi : nj;
    uint32_t bestS = 0xFFFFFFFFu;
    const int32_t j0 = bestJ;
    for (int32_t r = 0; r <= LITE_HOP / 4; r++) {
        const int32_t j = (r & 1) ? j0 + ((r + 1) & ~1) : j0 - (r & ~1);
        if (j < jlo || j > jhi) continue;
        uint32_t s = 0;
        if (fast) {
            const int8_t* c = d->cring;
            uint16_t q = ridx(d, j << 2) >> 2;
            for (int k = 0; k < LITE_CPTS && s < bestS; k++) {
                const int32_t e = c[q] - cref[k];
                s += (uint32_t)(e < 0 ? -e : e);
                if (++q == LITE_CRING) q = 0;
            }
        } else {
            for (int k = 0; k < LITE_CPTS && s < bestS; k++) {
                const int32_t e = coarse(d, j + k) - cref[k];
                s += (uint32_t)(e < 0 ? -e : e);
            }
        }
        if (s < bestS) { bestS = s; bestJ = j; }
    }
    // fine: every offset within one coarse step (+-7), comparing every 2nd sample at full rate
    int16_t fref[LITE_FPTS];
    for (int k = 0; k < LITE_FPTS; k++) fref[k] = sample(d, target + 2 * k);
    const int32_t c0 = (bestJ << 2) + (nominal & 3);
    int32_t best = c0; bestS = 0xFFFFFFFFu;
    for (int32_t r = 0; r <= 14; r++) {
        const int32_t c = (r & 1) ? c0 + ((r + 1) >> 1) : c0 - (r >> 1);
        uint32_t s = 0;
        if (fast) {
            uint16_t q = ridx(d, c);
            for (int k = 0; k < LITE_FPTS && s < bestS; k++) {
                const int32_t e = lite_ulaw[d->ring[q]] - fref[k];
                s += (uint32_t)(e < 0 ? -e : e);
                q += 2; if (q >= LITE_RING) q -= LITE_RING;
            }
        } else {
            for (int k = 0; k < LITE_FPTS && s < bestS; k++) {
                const int32_t e = sample(d, c + 2 * k) - fref[k];
                s += (uint32_t)(e < 0 ? -e : e);
            }
        }
        if (s < bestS) { bestS = s; best = c; }
    }
    return best;
}

static void nextHop(lite_deck* d)
{
    d->prev = d->cur;
    // ana += hop * speed (hop = 256: a shift)
    const uint32_t acc = (uint32_t)d->anaFrac + (((uint32_t)d->speed & 0xFFFF) << 8);
    d->ana += ((d->speed >> 16) << 8) + (int32_t)(acc >> 16);
    d->anaFrac = (uint16_t)acc;
    d->cur = d->fresh ? d->ana : lite_find(d, d->ana, d->prev + LITE_HOP);
    d->fresh = 0;
}

static inline __attribute__((always_inline)) int16_t stretchSample(lite_deck* d)
{
    if (d->i == 0) nextHop(d);
    const int32_t a = sample(d, d->prev + LITE_HOP + d->i), b = sample(d, d->cur + d->i);
    const int32_t y = a + (mul8(b - a, (uint8_t)d->i) >> 8);      // linear crossfade: rise = i / hop
    if (++d->i == LITE_HOP) d->i = 0;
    return (int16_t)y;
}

static inline __attribute__((always_inline)) int16_t varispeedSample(lite_deck* d)
{
    const int32_t a = sample(d, d->pos);
    int32_t y = a;
    const uint8_t f = (uint8_t)(d->posFrac >> 8);
    if (f) y = a + (mul8(sample(d, d->pos + 1) - a, f) >> 8);
    const uint32_t acc = (uint32_t)d->posFrac + ((uint32_t)d->speed & 0xFFFF);
    d->pos += (d->speed >> 16) + (int32_t)(acc >> 16);
    d->posFrac = (uint16_t)acc;
    return (int16_t)y;
}

// ---- deck ----

static int wantStretch(const lite_deck* d)
{
    const int32_t dev = d->speed > 0x10000 ? d->speed - 0x10000 : 0x10000 - d->speed;
    return d->keylock && d->speed >= 0x8000 && d->speed <= 0x20000 && dev > UNITY_BAND;
}

void lite_init(lite_deck* d, const lite_src* src)
{
    uint8_t* p = (uint8_t*)d;
    for (unsigned k = 0; k < sizeof *d; k++) p[k] = 0;
    d->src = *src;
    d->speed = 0x10000;
    jumpTo(d, 0);
}

int32_t lite_pos(const lite_deck* d)
{
    if (!d->stretch) return d->pos;
    // the analysis position advances hop * speed per hop; i samples into it
    return d->fresh ? d->cur + LITE_HOP : d->ana + (int32_t)(((uint32_t)d->i * (uint32_t)(d->speed >> 8)) >> 8);
}

void lite_seek(lite_deck* d, int32_t frame)
{
    d->seekTo = frame; d->pendingSeek = 1;
    if (d->playing) d->fadeDir = -1; else jumpTo(d, frame), d->pendingSeek = 0;
}

void lite_play(lite_deck* d, int on)
{
    if (on && !d->playing) { d->playing = 1; d->fade = 0; d->fadeDir = 1; }
    else if (on && d->fadeDir < 0 && !d->pendingSeek) d->fadeDir = 1;   // play again while stopping
    else if (!on && d->playing) d->fadeDir = -1;           // `playing` clears when the fade ends
}

void lite_set_speed(lite_deck* d, int32_t s) { d->speed = s < 0 ? 0 : s; }
void lite_set_keylock(lite_deck* d, int on) { d->keylock = (uint8_t)(on != 0); }

void lite_render(lite_deck* d, int16_t* out, int n)
{
    // Switching engines is a jump to where we are: fade out, switch, fade in.
    if (d->fadeDir == 0 && d->playing && wantStretch(d) != d->stretch) {
        d->seekTo = lite_pos(d); d->pendingSeek = 1; d->fadeDir = -1;
    }
    for (int k = 0; k < n; k++) {
        if (!d->playing) { out[k] = 0; continue; }
        int32_t y = d->stretch ? stretchSample(d) : varispeedSample(d);
        if (d->fadeDir) {
            if (d->fade < LITE_FADE) y = mul8(y, (uint8_t)(d->fade << 3)) >> 8;   // fade 0..31 -> gain 0..248/256
            if (d->fadeDir > 0) {
                if (++d->fade >= LITE_FADE) d->fadeDir = 0;
            } else if (d->fade == 0 || --d->fade == 0) {
                if (d->pendingSeek) {                             // faded out: jump, fade back in
                    jumpTo(d, d->seekTo); d->pendingSeek = 0;
                    d->stretch = (uint8_t)wantStretch(d);
                    d->fadeDir = 1;
                } else {
                    d->playing = 0; d->fadeDir = 0;
                }
            }
        } else {
            d->fade = LITE_FADE;
        }
        out[k] = (int16_t)y;
    }
}
