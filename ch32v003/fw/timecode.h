// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// timecode: Serato Control Vinyl / Control CD decoding, from stereo 16-bit PCM: speed, direction, bit lock and
// the position. freedj-esp's decoder (src/timecode.cpp: a clean-room take on xwax's algorithm; only the
// published format constants are data) in C, single-precision float (the FM-1's AC791N has an FPU: build with
// -mcpu=r3 -mfprev1 -ffp-contract=off; no doubles), for the FM-1's DVS.
//
// Per channel: a DC-tracking zero with hysteresis gives the crossings. Direction from which channel leads
// (the two carry the same sine a quarter turn apart); speed from the primary's half period against the
// nominal one (1 kHz); a bit at each secondary crossing (the primary's level loud or quiet against its
// running mean), predicted by the 20-bit LFSR: VALID_BITS right in a row is a lock.
//
// The position: the 20-bit window of bits names a cycle of the record (a table of 2^20 entries, 3 MiB). That
// table is the platform's (tc_lut): it may answer at once (the host's array), or later (-2: the FM-1 reads
// its block from the stick); the decoder counts the bits meanwhile. Once placed, a lock keeps the position
// by counting (+-1 a bit) and the table isn't asked again until the lock breaks (a needle drop).
#pragma once
#include <stdint.h>

// side B's code is side A's backwards (the taps reversed): read with side A's constants it "locks" only with
// TC_SWITCH_PHASE, on windows that aren't on the record (measured on the FM-1, 2026-11-26). Try both sides.
enum { TC_SERATO_VINYL, TC_SERATO_CD, TC_SERATO_VINYL_B };
enum { TC_SWITCH_PHASE = 1, TC_SWITCH_PRIMARY = 2, TC_SWITCH_POLARITY = 4 };
enum { TC_LEVEL_SAMPLE, TC_LEVEL_MEAN, TC_LEVEL_PEAK };

// the cycle index of a 20-bit window: >= 0, -1 not on the record, -2 not known yet (ask again)
typedef int32_t (*tc_lut)(uint32_t window, void* ctx);

typedef struct {
    uint8_t  positive, swapped;
    int32_t  zero;                                // the tracked DC, samples << 16
    uint32_t ticker;                              // samples since its last crossing
} tc_chan;

typedef struct {
    // the format
    uint32_t rate, bits, taps, seed, length, resolution, flags;
    float    zeroAlpha;                           // dt / (RC + dt)
    float    refPeriod;                           // the nominal period in samples
    // the signal
    tc_chan  pri, sec;
    uint32_t sincePri;
    uint8_t  forwards, locked;
    float    speed;                               // signed: 1.0 = 1x forwards
    int32_t  refLevel;
    uint32_t bitstream, expected, valid;
    // a bit's level: TC_LEVEL_SAMPLE the primary at the secondary's crossing (xwax, freedj-esp), or over the whole
    // primary half-cycle it falls in - its mean or its peak - called at that half-cycle's end
    uint8_t  levelMode, bitDue;
    int32_t  dueSample;
    uint32_t hcSum, hcN, hcPeak;
    // the position
    int32_t  position;                            // the cycle, -1 none
    tc_lut   lut;
    void*    lutCtx;
    uint8_t  pending;                             // a window asked of the table, not answered yet
    uint32_t pendWindow;
    int32_t  pendBits;                            // bits since (signed: the way the record went)
} tc_dec;

void     tc_begin(tc_dec* d, uint32_t rate, int format, tc_lut lut, void* ctx);
void     tc_reset(tc_dec* d);
void     tc_relock(tc_dec* d);                    // a needle drop: the bit history forgotten, the levels kept
void     tc_flags(tc_dec* d, uint32_t flags);
void     tc_level(tc_dec* d, int mode);           // TC_LEVEL_*: how a bit's level is measured (resets)
void     tc_push(tc_dec* d, const int16_t* lr, uint32_t frames);
uint32_t tc_length(int format);                   // the cycles on the record (A 712000, CD 950000, B 922000)
uint32_t tc_seed(int format);
uint32_t tc_next(int format, uint32_t window);    // the LFSR one step on (to build the table)
