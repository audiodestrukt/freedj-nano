// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nano/stretch.h"

#include <math.h>
#include <stdlib.h>

namespace nano {

int stretchHop(uint32_t rate) { return rate > 32000 ? 512 : 256; }

void StretchWindow::build(int hop)
{
    if (hop > kMaxHop) hop = kMaxHop;
    hop_ = hop;
    // sin^2 over the rising half of a 2*hop frame. fall = 32768 - rise, so the
    // two overlapping halves always sum to exactly unity gain.
    for (int i = 0; i < hop; i++) {
        float s = sinf(3.14159265f * 0.5f * (float)i / (float)hop);
        w_[i] = (uint16_t)lrintf(s * s * 32768.0f);
    }
}

void Stretch::reset(int32_t frame)
{
    const int hop = win_->hop();
    // The "previous" frame's second half starts at `frame`, so the first hop
    // fades that straight audio out while the first matched frame fades in.
    cur_ = frame - hop;
    prev_ = cur_;
    anaQ16_ = (int64_t)cur_ << 16;
    i_ = 0;
    fresh_ = true;
}

int32_t Stretch::find(const Pcm& pcm, int32_t nominal, int32_t target) const
{
    // Search +-1 hop (11.6 ms): bass needs up to half its period to line up (10 ms at 50 Hz).
    // At +-hop/4 (2.9 ms) everything under ~200 Hz couldn't align and followed the tempo,
    // 7.8 % sharp at +8 %. Two passes keep the cost down:
    //   coarse: every `cs`-th offset, comparing a whole hop sampled every `cs`-th (bass decides)
    //   fine:   every offset within one coarse step, comparing hop/2 every 2nd (detail decides)
    const int hop = win_->hop();
    const int search = hop;
    const int cs = hop / 64 < 2 ? 2 : hop / 64;              // 8 at 44.1 kHz, 4 at 22.05 kHz
    const int cpts = hop / cs, fpts = hop / 4;
    int32_t cref[64], fref[StretchWindow::kMaxHop / 4];
    for (int k = 0; k < cpts; k++) cref[k] = pcm.mono(target + cs * k);
    for (int k = 0; k < fpts; k++) fref[k] = pcm.mono(target + 2 * k);

    int32_t best = nominal;
    uint32_t bestS = UINT32_MAX;
    for (int32_t d = -search; d <= search; d += cs) {          // coarse
        uint32_t s = 0;
        for (int k = 0; k < cpts && s < bestS; k++) s += (uint32_t)abs(pcm.mono(nominal + d + cs * k) - cref[k]);
        if (s < bestS) { bestS = s; best = nominal + d; }
    }
    const int32_t c0 = best;
    bestS = UINT32_MAX;
    for (int32_t d = 1 - cs; d < cs; d++) {                    // fine
        uint32_t s = 0;
        for (int k = 0; k < fpts && s < bestS; k++) s += (uint32_t)abs(pcm.mono(c0 + d + 2 * k) - fref[k]);
        if (s < bestS) { bestS = s; best = c0 + d; }
    }
    return best;
}

void Stretch::nextHop(const Pcm& pcm, int32_t speedQ16)
{
    const int hop = win_->hop();
    prev_ = cur_;
    anaQ16_ += (int64_t)hop * speedQ16;
    lastSpeed_ = speedQ16;
    fresh_ = false;
    cur_ = find(pcm, (int32_t)(anaQ16_ >> 16), prev_ + hop);
}

void Stretch::render(const Pcm& pcm, int32_t speedQ16, int32_t* out, int n)
{
    const int hop = win_->hop();
    for (int k = 0; k < n; k++) {
        if (i_ == 0) nextHop(pcm, speedQ16);
        const int32_t rise = win_->rise(i_), fall = 32768 - rise;
        int32_t al, ar, bl, br;
        pcm.frame(prev_ + hop + i_, al, ar);
        pcm.frame(cur_ + i_, bl, br);
        out[2 * k]     = (al * fall + bl * rise) >> 15;
        out[2 * k + 1] = (ar * fall + br * rise) >> 15;
        if (++i_ == hop) i_ = 0;
    }
}

void varispeed(const Pcm& pcm, int64_t& posQ16, int32_t speedQ16, int32_t* out, int n)
{
    for (int k = 0; k < n; k++) {
        const int32_t f = (int32_t)(posQ16 >> 16);
        const int32_t frac = (int32_t)(posQ16 & 0xFFFF);
        int32_t al, ar, bl, br;
        pcm.frame(f, al, ar);
        pcm.frame(f + 1, bl, br);
        out[2 * k]     = al + (int32_t)(((int64_t)(bl - al) * frac) >> 16);
        out[2 * k + 1] = ar + (int32_t)(((int64_t)(br - ar) * frac) >> 16);
        posQ16 += speedQ16;
    }
}

} // namespace nano
