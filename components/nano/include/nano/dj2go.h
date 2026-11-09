// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Numark DJ2Go MIDI → deck events.
// Mapping from freedj-3000 docs/reference/dj2go-midi-map.md (Mixxx's
// "Numark DJ2Go.midi.xml", checked against the hardware 2026-08-26).
// Transport-agnostic: feed it 3-byte MIDI messages from USB host, ALSA, etc.

#include "nano/deck_event.h"

namespace dj2go {

class Decoder {
public:
    // Decode one MIDI message. Writes up to `max` events to `out` and returns
    // how many (0 for messages that aren't part of the map). Never more than 2.
    int decode(uint8_t status, uint8_t d1, uint8_t d2, deck::Event* out, int max);

    // Unwrap a USB-MIDI event packet (4 bytes: cable/CIN + 3 MIDI bytes).
    int decodeUsbPacket(const uint8_t p[4], deck::Event* out, int max);

private:
    // LOAD doubles as SHIFT: its Note On means "shift down", its Note Off
    // means "load". If another control was used while it was held, the
    // release is a shift release only and must not load.
    bool shiftHeld_[2]     = {false, false};
    bool shiftConsumed_[2] = {false, false};
};

} // namespace dj2go
