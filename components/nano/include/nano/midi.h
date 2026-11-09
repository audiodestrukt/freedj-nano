// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// MIDI byte stream -> complete channel messages. For DIN/TRS MIDI on a UART
// (31250 baud) or BLE-MIDI after its timestamp bytes are stripped.
// Handles running status, real-time bytes in the middle of a message, and
// skips SysEx. USB-MIDI packets don't need this (see dj2go::decodeUsbPacket).

#include <stdint.h>

namespace nano {

struct MidiMsg { uint8_t status, d1, d2; };

class MidiParser {
public:
    // Feed one byte. Returns true and fills `out` when a channel message is complete.
    bool feed(uint8_t b, MidiMsg& out);
    void reset() { status_ = 0; n_ = 0; sysex_ = false; }

private:
    uint8_t status_ = 0;   // running status (0 = none)
    uint8_t data_[2] = {};
    uint8_t n_ = 0;
    bool    sysex_ = false;
};

} // namespace nano
