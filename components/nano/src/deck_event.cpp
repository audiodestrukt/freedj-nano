// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nano/deck_event.h"

namespace deck {

const char* typeName(Type t)
{
    switch (t) {
    case Type::None:         return "none";
    case Type::PlayPause:    return "play/pause";
    case Type::Cue:          return "cue";
    case Type::Sync:         return "sync";
    case Type::LoopIn:       return "loop in";
    case Type::LoopOut:      return "loop out";
    case Type::Load:         return "load";
    case Type::Back:         return "back";
    case Type::Shift:        return "shift";
    case Type::Pfl:          return "pfl";
    case Type::Jog:          return "jog";
    case Type::Browse:       return "browse";
    case Type::TempoFader:   return "tempo";
    case Type::Volume:       return "volume";
    case Type::Crossfader:   return "crossfader";
    case Type::MasterVolume: return "master vol";
    case Type::PhonesVolume: return "phones vol";
    case Type::KeyLock:      return "key lock";
    case Type::HotCue:       return "hot cue";
    case Type::BeatLoop:     return "beat loop";
    case Type::ReloopExit:   return "reloop/exit";
    case Type::LoopHalve:    return "loop 1/2";
    case Type::LoopDouble:   return "loop x2";
    case Type::BeatJump:     return "beat jump";
    case Type::CueCall:      return "call";
    case Type::Quantize:     return "quantize";
    case Type::TempoRange:   return "tempo range";
    case Type::TempoReset:   return "tempo reset";
    case Type::MemoryAdd:    return "memory";
    case Type::MemoryDelete: return "delete memory";
    case Type::Slip:         return "slip";
    case Type::Reverse:      return "reverse";
    case Type::MeterView:    return "meter view";
    case Type::JogTouch:     return "jog touch";
    }
    return "?";
}

} // namespace deck
