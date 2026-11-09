// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// One deck: transport, cue, tempo, keylock, jog (nudge / scratch), on one core.
//
// Threads: post() from ONE control task (inputs, MIDI, touch, UI all live
// there); render() from the audio task, which applies the queued events at the
// start of each block. status() from anywhere (seqlock snapshot).
//
// Every jump (cue, load, play/stop, engine switch) is a crossfade over one
// block between the old and the new "voice", so there are no clicks and no
// special cases: a stopped voice simply renders silence.

#include <atomic>
#include <stdint.h>
#include "nano/deck_event.h"
#include "nano/library.h"
#include "nano/stretch.h"

namespace nano {

struct DeckStatus {
    int      track = -1;
    int64_t  posQ16 = 0;       // playback position, Q16 frames
    uint32_t frames = 0, rate = 0;
    uint32_t cue = 0;          // cue point, frames
    int32_t  speedQ16 = 0;     // what's playing now (0 when stopped)
    int16_t  pitchPermille = 0;  // tempo fader, 1/10 %: 25 = +2.5 %
    uint8_t  rangePct = 8;     // tempo range, +-%
    bool     playing = false, cueing = false, keylock = false, scratching = false;
    bool     stretching = false;   // keylock engine active this block
};

class Deck {
public:
    static constexpr int kBlock = 256;       // max frames per render() call

    explicit Deck(const Library& lib) : lib_(lib) {}

    void post(const deck::Event& e);         // control task only
    void render(int16_t* out, int n);        // audio task; n <= kBlock stereo frames
    DeckStatus status() const;
    uint32_t rate() const { return rate_.load(std::memory_order_relaxed); }   // current track's

private:
    struct Voice {
        Pcm     pcm;
        bool    stretch = false;
        int64_t posQ16 = 0;
        int32_t speedQ16 = 0;
        Stretch st;
        int64_t pos() const { return stretch ? st.posQ16() : posQ16; }
        void render(int32_t* out, int n);
    };

    const Library& lib_;
    StretchWindow win_;

    // event queue: single producer (control), single consumer (audio)
    static constexpr int kQ = 64;
    deck::Event q_[kQ];
    std::atomic<uint32_t> qHead_{0}, qTail_{0};

    // audio-task state
    Track   track_;
    int     trackIndex_ = -1;
    Voice   v_;
    bool    playing_ = false, cueing_ = false, keylock_ = false, touch_ = false;
    uint32_t cue_ = 0;
    int32_t pitchQ16_ = 0;        // tempo offset from 1.0
    int16_t pitchPermille_ = 0;
    uint8_t rangePct_ = 8;
    int32_t nudgeQ16_ = 0;        // jog while playing: temporary speed offset, decays
    int64_t scratchTarget_ = 0;   // jog while touched or stopped: where the platter is
    bool    scratching_ = false;
    int     scratchIdle_ = 0;     // blocks since the last jog tick
    int32_t gainQ15_ = 32768;
    bool    jump_ = false;        // crossfade this block
    int32_t a_[kBlock * 2], b_[kBlock * 2];
    std::atomic<uint32_t> rate_{0};

    // status seqlock
    std::atomic<uint32_t> seq_{0};
    DeckStatus st_;

    void apply(const deck::Event& e);
    void load(int index);
    void seek(uint32_t frame);
    int32_t targetSpeed();
    void publish(const Voice& v);
};

} // namespace nano
