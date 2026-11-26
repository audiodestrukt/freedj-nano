// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// vinyl: when the timecode record has the deck, and how the deck keeps to the needle. Pure logic over the
// decoder (fw/timecode.h), no hardware, host tested (sim/vinyl_test.c). freedj-esp's src/vinyl_rules.h in C,
// single-precision (the FM-1: no doubles), times in wrap-safe uint32 microseconds.
//
// The rules as freedj-3000's DVS, three of them fixed on a turntable (2026-11-24):
// - A hand holding the platter between strokes is not a needle lift: a lift is 500 ms without carrier, and a
//   lift only clears the decoder's bit history (tc_relock), so the next position comes in ~45 cycles.
// - ABS jumps only on a needle drop, the first lock, or a >= 300 ms gap once the record has played steadily
//   for 300 ms. Smaller gaps, once steady, close by bending the speed (<= 2 %); while the platter is handled
//   the deck follows speed alone.
#pragma once
#include <stdint.h>
#include "timecode.h"

// ── The record: armed, signal, settling ──
enum {
    VINYL_MIN_PEAK = 600,                         // about -35 dBFS
    VINYL_ARM_MS = 300,                           // steady carrier near 1x this long arms it
    VINYL_DROP_MS = 40,                           // no carrier this long = record stopped
    VINYL_LIFT_MS = 500,                          // no carrier this long = needle lifted
    VINYL_SETTLE_MAX_MS = 1000,
};
#define VINYL_MAX_SPEED 3.0f
#define VINYL_MIN_SPEED 0.02f

typedef struct {
    uint8_t  armed, signal, settling;
    float    speed;                               // signed; 0 with no signal or while settling
    uint32_t drops;                               // needle drops placed (settling ended on a lock)
    // this window's events, for logging
    uint8_t  landed, placed, gaveUp, armedNow;    // a drop began / ended placed / gave up; armed just now
    int32_t  settleMs;
} vinyl_rec;

typedef struct {
    vinyl_rec r;
    uint32_t  lastCarrierUs, steadySinceUs, landedUs;
    uint8_t   haveCarrier, steady;
} vinyl_win;

void             vinyl_win_init(vinyl_win* w);
// once per ~5-10 ms of input: now, that window's loudest sample, the decoder that has just read it, and
// whether needle drops settle (ABS)
const vinyl_rec* vinyl_step(vinyl_win* w, uint32_t nowUs, int16_t peak, tc_dec* dec, int absolute);

// ── ABS: keeping the deck on the needle ──
#define VINYL_SEEK_MS     300.0f                  // a gap this big jumps (once steady)
#define VINYL_SEEK_GAP_MS 250.0f                  // at most this often
#define VINYL_STEADY_MS   300.0f                  // 0.5-2x, the reading within 3 % of the smoothed speed
#define VINYL_DEAD_MS     4.0f                    // a gap under this is left alone
#define VINYL_BEND_TAU_MS 800.0f                  // the bend closes a gap this fast...
#define VINYL_BEND_MAX    0.02f                   // ...at most this much

typedef struct {
    uint8_t seek, drop;                           // jump to seekMs; the jump is a needle drop (or first lock)
    float   seekMs;                               // ms of track
    float   bend;                                 // add to the deck's speed
} vinyl_out;

typedef struct {
    float    steadyMs, sinceSeekMs;
    uint8_t  placed;
    uint32_t dropsSeen;
} vinyl_follow;

void      vinyl_follow_init(vinyl_follow* f);     // also: the record lost the deck, place it afresh
// one block of dtMs: the record's raw and the deck's smoothed speed; the needle's target (ms of track, the
// output lead included) when ABS has a position; the deck's playhead (ms); the drops count (vinyl_rec)
vinyl_out vinyl_follow_step(vinyl_follow* f, float raw, float smooth, float dtMs, int hasTarget, float targetMs,
                            float deckMs, uint32_t drops);

// ── Which record, which way round: side A or B, and the decoder flags this input chain needs ──
// Every combination (2 sides x 8 flags) read at once until one has been locked with the speed positive for
// VINYL_SCAN_MS in all (lock breaks allowed: a real record misreads a bit now and then) on a record playing ~1x (nobody holds a record backwards at 33). The wrong side only ever locks
// backwards (side B is side A's code reversed), so that picks the side too. 16 decoders: ~37 % of the FM-1
// while it runs, about half a second; then one decoder. (POLARITY only moves the sampling to the other half
// cycle: with or without it the same bits, the same position; the lower flags win.) Host tested: sim/vinyl_test.c.
#define VINYL_SCAN_N  16
#define VINYL_SCAN_MS 300
typedef struct {
    tc_dec   dec[VINYL_SCAN_N];                   // [side * 8 + flags], side 0 = A, 1 = B
    uint32_t goodUs[VINYL_SCAN_N];                // locked forwards, in all (us)
    int8_t   found;                               // the winner's index, -1 still looking
} vinyl_scan;
void vinyl_scan_begin(vinyl_scan* s, uint32_t rate);
// a block of input at nowUs (nonzero): 1 once found (vinyl_scan_side / _flags then say which)
int  vinyl_scan_push(vinyl_scan* s, const int16_t* lr, uint32_t frames, uint32_t nowUs);
int  vinyl_scan_side(const vinyl_scan* s);        // TC_SERATO_VINYL or TC_SERATO_VINYL_B
uint32_t vinyl_scan_flags(const vinyl_scan* s);
