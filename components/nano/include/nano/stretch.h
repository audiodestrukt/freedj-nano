// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Lightweight keylock: WSOLA in fixed point, reading straight from the track.
//
// freedj-esp's WSOLA (src/wsola.cpp) pushes decoded audio through input and
// overlap-add buffers (~20 KB). Here the whole track is random-access PCM
// (memory-mapped flash), so there's nothing to buffer: each output sample is
//
//     out[i] = fall[i] * src[prev + hop + i] + rise[i] * src[cur + i]
//
// two frames at 50 % overlap, a sin^2 window whose halves sum to exactly 1.
// `cur` is the analysis position (advancing hop * speed per hop), moved by up
// +-1 hop (11.6 ms, so bass lines up) to the offset whose start best matches what `prev` would have
// played next (AMDF on mono: a coarse pass over a whole hop, then a fine one).
// State is a handful of ints plus a shared window table: ~40 bytes per voice.
//
// Cost: measured 8 % of a 160 MHz C6 at 22.05 kHz mono, keylocked at +8 %.

#include <stdint.h>
#include "nano/pcm.h"

namespace nano {

class StretchWindow {
public:
    static constexpr int kMaxHop = 512;
    void build(int hop);             // uses float once, at load time
    int hop() const { return hop_; }
    uint16_t rise(int i) const { return w_[i]; }   // Q15, 0 .. 32768
private:
    int hop_ = 0;
    uint16_t w_[kMaxHop] = {};
};

// Hop for a sample rate: ~11.6 ms (512 at 44.1 kHz, 256 at 22.05 kHz).
int stretchHop(uint32_t rate);

class Stretch {
public:
    void begin(const StretchWindow* w) { win_ = w; }
    void reset(int32_t frame);       // start playing at this frame
    // Render n stereo frames (interleaved int32, not yet saturated) at `speed` (Q16, > 0).
    void render(const Pcm& pcm, int32_t speedQ16, int32_t* out, int n);
    // Smooth playback position (Q16 frames), for the display and engine hand-over.
    int64_t posQ16() const {
        return fresh_ ? (int64_t)(cur_ + win_->hop()) << 16 : anaQ16_ + (int64_t)i_ * lastSpeed_;
    }

private:
    const StretchWindow* win_ = nullptr;
    int64_t anaQ16_ = 0;    // nominal start of the current frame
    int32_t prev_ = 0;      // chosen start of the previous frame
    int32_t cur_ = 0;       // chosen start of the current frame
    int     i_ = 0;         // index within the hop
    int32_t lastSpeed_ = kOne;
    bool    fresh_ = true;  // reset, no hop rendered yet

    void nextHop(const Pcm& pcm, int32_t speedQ16);
    int32_t find(const Pcm& pcm, int32_t nominal, int32_t target) const;
};

// Plain varispeed (pitch follows speed): linear interpolation at a Q16
// position. Any speed, including reverse and the slow crawl of a scratch.
void varispeed(const Pcm& pcm, int64_t& posQ16, int32_t speedQ16, int32_t* out, int n);

} // namespace nano
