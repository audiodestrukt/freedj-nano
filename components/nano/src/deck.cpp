// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nano/deck.h"

#include <string.h>

namespace nano {

using deck::Type;

// Feel: one place to tune (cf. freedj-esp's README feel table).
static constexpr int32_t kNudgePerTick = kOne / 200;     // jog while playing: +-0.5 % per tick
static constexpr int32_t kNudgeMax     = kOne / 5;       // +-20 %
static constexpr int     kScratchMsPerTick = 4;          // jog while touched/stopped: platter travel per tick
static constexpr int     kScratchIdleBlocks = 8;         // stopped scrub ends this long after the last tick
static constexpr int32_t kStretchMin = kOne / 2, kStretchMax = kOne * 2;   // keylock range
static constexpr int32_t kUnityBand = 64;                // within ~0.1 % of 1.0: play straight
static const uint8_t kRanges[] = {6, 10, 16, 50};

void Deck::Voice::render(int32_t* out, int n)
{
    if (!speedQ16 || !pcm.valid()) { memset(out, 0, sizeof(int32_t) * 2 * n); return; }
    if (stretch) st.render(pcm, speedQ16, out, n);
    else varispeed(pcm, posQ16, speedQ16, out, n);
}

void Deck::post(const deck::Event& e)
{
    const uint32_t t = qTail_.load(std::memory_order_relaxed), next = (t + 1) % kQ;
    if (next == qHead_.load(std::memory_order_acquire)) return;   // full: drop (control is way ahead)
    q_[t] = e;
    qTail_.store(next, std::memory_order_release);
}

void Deck::seek(uint32_t frame)
{
    if (v_.stretch) v_.st.reset((int32_t)frame);
    else v_.posQ16 = (int64_t)frame << 16;
    jump_ = true;
}

void Deck::load(int index)
{
    Track t;
    if (!lib_.get(index, t)) return;
    track_ = t; trackIndex_ = index;
    const int hop = stretchHop(t.pcm.rate);
    if (hop != win_.hop()) win_.build(hop);
    v_ = Voice{};
    v_.pcm = t.pcm;
    v_.st.begin(&win_);
    playing_ = cueing_ = scratching_ = touch_ = false;
    cue_ = 0; nudgeQ16_ = 0;
    rate_.store(t.pcm.rate, std::memory_order_relaxed);
    jump_ = true;
}

void Deck::apply(const deck::Event& e)
{
    const uint32_t pos = (uint32_t)((v_.pos() < 0 ? 0 : v_.pos()) >> 16);
    switch (e.type) {
    case Type::Load: load(e.delta); break;
    case Type::PlayPause:
        if (!e.pressed) break;
        if (cueing_) { cueing_ = false; playing_ = true; }   // CDJ: PLAY during a cue preview keeps playing
        else playing_ = !playing_;
        break;
    case Type::Cue:
        if (e.pressed) {
            if (playing_) { playing_ = false; seek(cue_); }        // back to the cue and pause
            else { if (pos != cue_) cue_ = pos; cueing_ = true; }  // set the cue here, preview while held
        } else if (cueing_) {
            cueing_ = false; seek(cue_);
        }
        break;
    case Type::KeyLock: if (e.pressed) keylock_ = !keylock_; break;
    case Type::TempoFader: {
        pitchPermille_ = (int16_t)((e.position - 0.5f) * 2.0f * rangePct_ * 10.0f);
        pitchQ16_ = pitchPermille_ * kOne / 1000;
        break;
    }
    case Type::TempoRange:
        if (!e.pressed) break;
        for (unsigned i = 0; i < sizeof kRanges; i++)
            if (kRanges[i] == rangePct_) { rangePct_ = kRanges[(i + 1) % sizeof kRanges]; break; }
        break;
    case Type::TempoReset: if (e.pressed) { pitchPermille_ = 0; pitchQ16_ = 0; } break;
    case Type::JogTouch:
        touch_ = e.pressed;
        scratching_ = e.pressed;           // hand on the platter holds it still
        scratchTarget_ = v_.pos();
        break;
    case Type::Jog:
        if (touch_ || !(playing_ || cueing_)) {
            if (!scratching_) { scratching_ = true; scratchTarget_ = v_.pos(); }
            scratchTarget_ += (int64_t)e.delta * ((int64_t)track_.pcm.rate * kScratchMsPerTick / 1000) << 16;
            scratchIdle_ = 0;
        } else {
            nudgeQ16_ += e.delta * kNudgePerTick;
            if (nudgeQ16_ > kNudgeMax) nudgeQ16_ = kNudgeMax;
            if (nudgeQ16_ < -kNudgeMax) nudgeQ16_ = -kNudgeMax;
        }
        break;
    case Type::Volume: case Type::MasterVolume:
        gainQ15_ = (int32_t)(e.position * 32768.0f);
        break;
    default: break;   // Browse etc. belong to the app
    }
}

int32_t Deck::targetSpeed()
{
    if (scratching_) {
        // Chase the platter: cover the distance in about two blocks.
        int64_t d = (scratchTarget_ - v_.pos()) / (2 * kBlock);
        if (d > 4 * kOne) d = 4 * kOne;
        if (d < -4 * kOne) d = -4 * kOne;
        return (int32_t)d;
    }
    if (!(playing_ || cueing_)) return 0;
    return kOne + pitchQ16_ + nudgeQ16_;
}

void Deck::render(int16_t* out, int n)
{
    if (n > kBlock) n = kBlock;
    Voice old = v_;
    const bool hadWindow = win_.hop();
    const int oldHop = win_.hop();
    jump_ = false;

    uint32_t h = qHead_.load(std::memory_order_relaxed);
    while (h != qTail_.load(std::memory_order_acquire)) {
        apply(q_[h]);
        h = (h + 1) % kQ;
        qHead_.store(h, std::memory_order_release);
    }
    if (hadWindow && win_.hop() != oldHop && old.stretch) {   // its window changed under it: let it go straight
        old.posQ16 = old.pos(); old.stretch = false;
    }

    const int32_t speed = targetSpeed();
    const int32_t dev = speed > kOne ? speed - kOne : kOne - speed;
    const bool wantStretch = keylock_ && !scratching_ && speed >= kStretchMin && speed <= kStretchMax && dev > kUnityBand;
    if (wantStretch != v_.stretch && v_.pcm.valid()) {
        const int64_t p = v_.pos();
        v_.stretch = wantStretch;
        if (wantStretch) v_.st.reset((int32_t)(p >> 16)); else v_.posQ16 = p;
        jump_ = true;
    }
    if ((old.speedQ16 == 0) != (speed == 0)) jump_ = true;    // start or stop: fade
    v_.speedQ16 = speed;

    if (jump_) {
        old.render(a_, n);
        v_.render(b_, n);
        for (int k = 0; k < n; k++) {
            const int32_t w = (k << 15) / n;
            for (int c = 0; c < 2; c++)
                a_[2 * k + c] = (a_[2 * k + c] * (32768 - w) + b_[2 * k + c] * w) >> 15;
        }
    } else {
        v_.render(a_, n);
    }
    for (int i = 0; i < 2 * n; i++) out[i] = sat16((int32_t)(((int64_t)a_[i] * gainQ15_) >> 15));

    nudgeQ16_ -= nudgeQ16_ / 8;                                  // nudge springs back
    if (scratching_ && !touch_ && ++scratchIdle_ > kScratchIdleBlocks) scratching_ = false;
    if ((playing_ || cueing_) && v_.pcm.valid() && v_.pos() >= ((int64_t)v_.pcm.frames << 16))
        playing_ = cueing_ = false;                              // end of track
    publish(v_);
}

void Deck::publish(const Voice& v)
{
    DeckStatus s;
    s.track = trackIndex_;
    s.posQ16 = v.pos() < 0 ? 0 : v.pos();
    s.frames = v.pcm.frames; s.rate = v.pcm.rate; s.cue = cue_;
    s.speedQ16 = v.speedQ16;
    s.pitchPermille = pitchPermille_; s.rangePct = rangePct_;
    s.playing = playing_; s.cueing = cueing_; s.keylock = keylock_;
    s.scratching = scratching_; s.stretching = v.stretch;
    seq_.fetch_add(1, std::memory_order_acq_rel);    // odd: writing
    st_ = s;
    seq_.fetch_add(1, std::memory_order_release);    // even: done
}

DeckStatus Deck::status() const
{
    DeckStatus s;
    for (;;) {
        const uint32_t a = seq_.load(std::memory_order_acquire);
        if (a & 1) continue;
        s = st_;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) == a) return s;
    }
}

} // namespace nano
