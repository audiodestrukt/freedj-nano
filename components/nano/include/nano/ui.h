// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The deck screen, laid out by screen size:
//   128x32 mono    name, waveform strip, time + tempo
//   128x64 mono    + track count, overview bar, transport state
//   color >= 240   header, big time, zoomed + overview waveforms, touch buttons
// Same Model on every backend; draw() is pure, so the host renders it to files.

#include <stdint.h>
#include "nano/deck.h"
#include "nano/gfx.h"

namespace ui {

struct Model {
    nano::DeckStatus st;
    const nano::Track* track = nullptr;   // loaded track (peaks, name), or null
    int  tracks = 0;
    int  selected = 0;                    // browse cursor
    bool browsing = false;                // show the track list
    const char* (*nameOf)(int i, void* ctx) = nullptr;
    void* ctx = nullptr;
};

void draw(gfx::Surface& s, const Model& m);

// Touch on the color layouts: buttons along the bottom, drag on the waveform
// to scratch (JogTouch + Jog), tap a row in the list to load it.
class Touch {
public:
    // One touch sample (down = finger on the glass). Writes up to `max` events.
    int feed(const Model& m, int w, int h, bool down, int x, int y, deck::Event* out, int max);
private:
    enum Hit : uint8_t { None, Cue, Play, Key, Next, Wave, List };
    Hit active_ = None;
    int lastX_ = 0, acc_ = 0;
};

} // namespace ui
