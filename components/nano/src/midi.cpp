// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nano/midi.h"

namespace nano {

static int dataBytes(uint8_t status)
{
    switch (status & 0xF0) {
    case 0xC0: case 0xD0: return 1;   // program change, channel pressure
    default:              return 2;
    }
}

bool MidiParser::feed(uint8_t b, MidiMsg& out)
{
    if (b >= 0xF8) return false;                 // real-time: ignore, don't disturb the message
    if (b == 0xF0) { sysex_ = true; status_ = 0; n_ = 0; return false; }
    if (b == 0xF7) { sysex_ = false; return false; }
    if (b & 0x80) {
        sysex_ = false;
        if (b >= 0xF0) { status_ = 0; n_ = 0; return false; }   // system common: cancels running status
        status_ = b; n_ = 0;
        return false;
    }
    if (sysex_ || !status_) return false;        // SysEx payload, or data with no status
    data_[n_++] = b;
    if (n_ < dataBytes(status_)) return false;
    out.status = status_;
    out.d1 = data_[0];
    out.d2 = n_ > 1 ? data_[1] : 0;
    n_ = 0;                                      // keep status_: running status
    return true;
}

} // namespace nano
