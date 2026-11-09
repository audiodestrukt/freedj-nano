// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Deck control events: the one vocabulary every input source speaks
// (copied from freedj-esp src/deck_event.h, 2026-11-08; JogTouch added)
// (DJ2Go MIDI, the A1S buttons/encoder, serial keys, a future I2C panel).
// Names follow freedj-3000's ControlEvent so a control panel can drive both.
// Plain C++ — no Arduino or ESP-IDF headers — so it builds on host and target.

#include <stdint.h>

namespace deck {

enum class Type : uint8_t {
    None,
    // Buttons — `pressed` is true on press, false on release.
    PlayPause,
    Cue,          // momentary: hold-to-preview semantics live in the player
    Sync,
    LoopIn,
    LoopOut,
    Load,         // deck A/B: load selection to that deck; Any: to the active deck
    Back,         // browser up a level
    Shift,
    Pfl,          // headphone cue select
    // Relative — `delta` is signed detents/ticks since the last event.
    Jog,
    Browse,
    // Absolute — `position` is 0.0..1.0.
    TempoFader,   // 0.5 = centre (0 %); which end is "faster" is the player's call
    Volume,
    Crossfader,   // 0 = full A, 1 = full B
    MasterVolume,
    PhonesVolume,
    // Buttons, added later (kept last so existing values do not move).
    KeyLock,
    // Performance controls. `delta` carries the argument.
    HotCue,       // delta = 1..8 (A..H); position = 1 deletes it
    BeatLoop,     // position = loop length in beats (0.5 .. 16)
    ReloopExit,   // exit an active loop, or re-enter the last one
    LoopHalve,
    LoopDouble,
    BeatJump,     // delta = beats, signed
    CueCall,      // delta = -1 previous / +1 next memory cue
    Quantize,     // toggle
    TempoRange,   // cycle the pitch range (±6/10/16/WIDE)
    TempoReset,   // toggle: play at 0 % regardless of the fader
    MemoryAdd,    // store the cue point as a memory cue
    MemoryDelete, // delete the memory cue at the cue point
    Slip,         // toggle slip mode
    Reverse,      // toggle reverse
    MeterView,    // toggle the beat meter between bar boxes and phase ticks (UI only)
    // freedj-nano additions (kept last, as above).
    JogTouch,     // pressed = hand on the platter / finger on the waveform: Jog scratches
};

enum : uint8_t { DeckA = 0, DeckB = 1, DeckAny = 0xFF };

// Load: delta = track index (the app turns a bare Load into the browsed index).
struct Event {
    Type    type     = Type::None;
    uint8_t deck     = DeckAny;
    bool    pressed  = false;
    int16_t delta    = 0;
    float   position = 0.0f;
};

const char* typeName(Type t);

} // namespace deck
