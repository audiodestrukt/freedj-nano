// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A track as 16-bit PCM in memory: a memory-mapped flash partition on the
// target, a plain array on the host. Random access, no decoder state, so the
// time-stretch can read wherever it likes.
//
// Fixed point throughout (the C6 has no FPU): positions and speeds are Q16.16.

#include <stdint.h>

namespace nano {

constexpr int32_t kOne = 1 << 16;   // Q16.16 unity (speed 1.0)

inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

struct Pcm {
    const int16_t* data = nullptr;   // interleaved, `channels` per frame
    uint32_t frames = 0;
    uint32_t rate = 0;
    uint8_t  channels = 1;           // 1 or 2

    bool valid() const { return data && frames && rate; }

    // One frame as stereo; silence outside the track.
    inline void frame(int32_t f, int32_t& l, int32_t& r) const {
        if (f < 0 || (uint32_t)f >= frames) { l = r = 0; return; }
        const int16_t* p = data + (uint32_t)f * channels;
        l = p[0];
        r = channels > 1 ? p[1] : p[0];
    }
    inline int32_t mono(int32_t f) const {
        int32_t l, r; frame(f, l, r); return (l + r) >> 1;
    }
};

} // namespace nano
