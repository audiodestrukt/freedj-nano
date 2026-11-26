// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// nano-hifi: the C6 engine (components/nano: deck.cpp's voices, stretch.cpp) in plain C, for the chips with the
// memory for it: stereo 16-bit PCM at the track's own rate (any; a hop of 512 above 32 kHz, else 256), keylock as
// WSOLA with a sin^2 window whose halves sum to exactly 1, varispeed by linear interpolation at a Q16.16 position.
// Every jump (seek, start, stop, engine switch) is a crossfade over one block between the old and the new voice.
//
// lite's interface (lite/lite.h), so fw/app.c drives either: transport only (tempo, nudge, scrub and the end of
// the track are the app's). Fixed point in the audio path, no libc, no libm (the window is built once with its
// own sine), no allocation: a deck is 5.2 KB (a 1 KB window, two 2 KB mixing blocks; lite_deck is 1 KB), plus
// 0.8 KB of stack in the keylock search. 2.2 KB of code on the FM-1. Single threaded: call
// everything from one task, or hand the calls to the audio task (render() applies what was set since the last).
//
// Tested sample for sample against the C++ engine (ch32v003/tests/test_hifi.cpp).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define HIFI_BLOCK   256                          // max frames per hifi_render()
#define HIFI_MAX_HOP 512
#define HIFI_ONE     65536                        // Q16.16 unity

// A track as interleaved 16-bit PCM in memory (memory-mapped flash on the target): random access
typedef struct {
    const int16_t* data;
    uint32_t frames, rate;
    uint8_t  channels;                            // 1 or 2
} hifi_pcm;

typedef struct {
    int64_t anaQ16;                               // nominal start of the current frame
    int32_t prev, cur;                            // chosen starts of the previous and current frames
    int32_t lastSpeed;
    uint16_t i;                                   // index within the hop
    uint8_t fresh;                                // reset, no hop rendered yet
} hifi_stretch;

typedef struct {
    uint8_t      stretch;                         // engine: 1 keylock, 0 varispeed
    int64_t      posQ16;                          // varispeed's position
    int32_t      speed;                           // Q16, what it plays at (0 silent)
    hifi_stretch st;
} hifi_voice;

typedef struct {
    hifi_pcm   pcm;
    uint16_t   hop;
    uint16_t   win[HIFI_MAX_HOP];                 // sin^2 rise, Q15 0..32768
    hifi_voice v;
    int32_t    speed;                             // set: Q16, signed (< 0 backwards: varispeed)
    uint8_t    playing, keylock;
    uint8_t    seekPending;
    int32_t    seekTo;
    uint8_t    jump;
    int32_t    gainQ15;
    int32_t    a[HIFI_BLOCK * 2], b[HIFI_BLOCK * 2];
} hifi_deck;

void    hifi_init(hifi_deck* d, const hifi_pcm* pcm);  // a track (stopped at 0); data 0: silence
void    hifi_play(hifi_deck* d, int on);
void    hifi_set_speed(hifi_deck* d, int32_t speedQ16);
void    hifi_set_keylock(hifi_deck* d, int on);   // keylock between 0.5x and 2x, straight within 0.1 % of 1x
void    hifi_seek(hifi_deck* d, int32_t frame);   // crossfaded at the next render
void    hifi_set_gain(hifi_deck* d, int32_t gainQ15);
int32_t hifi_pos(const hifi_deck* d);             // frames
int64_t hifi_pos_q16(const hifi_deck* d);
int     hifi_stretching(const hifi_deck* d);      // the keylock engine is the one playing
void    hifi_render(hifi_deck* d, int16_t* out, int n);   // n <= HIFI_BLOCK stereo frames, interleaved

#ifdef __cplusplus
}
#endif
