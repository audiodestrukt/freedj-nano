// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// nano-lite: freedj-nano's deck engine cut down for the CH32V003 (RV32EC, 48 MHz,
// no multiply, 2 KB RAM). Plain C, no division, no float, no allocation.
//
// The C6 engine (components/nano) memory-maps the whole track. Here the track
// streams forward from storage (SD or SPI flash) through two small rings:
//   ring   768 x mu-law bytes, full rate (35 ms at 22.05 kHz): what's played
//   cring  192 x int8, the same 35 ms decimated 4:1: what the keylock's coarse search reads
// Everything the engine touches stays near the playhead, so reads are sequential;
// only jumps (cue, load) seek. A read outside the rings counts in `misses`: the
// host tests require 0.
//
// Keylock is the C6's WSOLA: two frames at 50 % overlap, but a linear crossfade
// (the halves still sum to 1, one 8-bit multiply per sample) and the +-1 hop
// search in two passes, coarse on the decimated ring, fine on the full one.
// Mono, 22.05 kHz.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LITE_HOP    256    // 11.6 ms at 22.05 kHz
#define LITE_RING   768    // full-rate ring: 35 ms at 22.05 kHz (a multiple of 32)
#define LITE_CRING  (LITE_RING / 4)   // decimated ring, in step with the full one
#define LITE_FADE   32     // samples of fade around jumps, starts and stops
#define LITE_CPTS   32     // coarse compare: 32 decimated points = hop/2
#define LITE_FPTS   32     // fine compare: 32 points, every 2nd sample

extern const int16_t lite_ulaw[256];
uint8_t lite_ulaw_encode(int16_t s);

typedef struct {
    // Copy n mu-law bytes of the track starting at byte (= frame) `off`. Sequential
    // between seeks; n <= 32.
    void (*read)(void* ctx, uint32_t off, uint8_t* dst, uint8_t n);
    void* ctx;
    uint32_t frames;
} lite_src;

typedef struct {
    lite_src src;
    // rings
    uint8_t  ring[LITE_RING];
    int8_t   cring[LITE_CRING];
    int32_t  head;           // next frame to fetch (a multiple of 4)
    int32_t  rbase;          // ring index origin: moves up by LITE_RING, under 2 rings behind head
    int32_t  base;           // first frame fetched since the last seek
    uint16_t misses;
    // transport
    int32_t  speed;          // Q16, >= 0
    uint8_t  playing, keylock;
    uint8_t  stretch;        // engine in use: 1 = keylock, 0 = varispeed
    int8_t   fadeDir;        // -1 fading out, +1 fading in, 0 steady
    uint8_t  fade;           // 0..LITE_FADE
    uint8_t  pendingSeek;
    int32_t  seekTo;
    // varispeed
    int32_t  pos; uint16_t posFrac;
    // keylock
    int32_t  prev, cur, ana; uint16_t anaFrac; uint16_t i; uint8_t fresh;
} lite_deck;

void    lite_init(lite_deck* d, const lite_src* src);
void    lite_play(lite_deck* d, int on);
void    lite_set_speed(lite_deck* d, int32_t speedQ16);
void    lite_set_keylock(lite_deck* d, int on);
void    lite_seek(lite_deck* d, int32_t frame);        // fades out, jumps, fades in
int32_t lite_pos(const lite_deck* d);                  // frames
void    lite_render(lite_deck* d, int16_t* out, int n);

// For the tests and the bench.
int32_t lite_find(lite_deck* d, int32_t nominal, int32_t target);

// PWM: a sample as two 8-bit duty cycles (high and low byte, summed 256:1 by resistors).
static inline void lite_pwm(int16_t s, uint8_t* hi, uint8_t* lo)
{
    const uint16_t u = (uint16_t)(s + 32768);
    *hi = (uint8_t)(u >> 8);
    *lo = (uint8_t)u;
}

#ifdef __cplusplus
}
#endif
