// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nano/dj2go.h"

namespace dj2go {

using deck::Event;
using deck::Type;
using deck::DeckA;
using deck::DeckB;
using deck::DeckAny;

namespace {

struct NoteMap { uint8_t note; Type type; uint8_t deck; };
struct CcMap   { uint8_t cc;   Type type; uint8_t deck; bool relative; bool invert; };

// Buttons that send Note On on press and Note Off (or Note On vel 0) on release.
const NoteMap kNotes[] = {
    {0x3B, Type::PlayPause, DeckA}, {0x42, Type::PlayPause, DeckB},
    {0x33, Type::Cue,       DeckA}, {0x3C, Type::Cue,       DeckB},
    {0x40, Type::Sync,      DeckA}, {0x47, Type::Sync,      DeckB},
    // Pitch bend -/+ buttons (the DJ2Go has no loop buttons): - = key lock, + = beat meter view.
    {0x43, Type::KeyLock,   DeckA}, {0x45, Type::KeyLock,   DeckB},
    {0x44, Type::MeterView, DeckA}, {0x46, Type::MeterView, DeckB},
    {0x65, Type::Pfl,       DeckA}, {0x66, Type::Pfl,       DeckB},
};

// LOAD A/B: Note On = shift down, Note Off = load (unless used as shift).
constexpr uint8_t kLoadA = 0x4B, kLoadB = 0x34;
// BACK / ENTER only send Note Off.
constexpr uint8_t kBack = 0x59, kEnter = 0x5A;

const CcMap kCcs[] = {
    {0x19, Type::Jog,          DeckA,   true,  false},
    {0x18, Type::Jog,          DeckB,   true,  false},
    {0x1A, Type::Browse,       DeckAny, true,  false},
    {0x0D, Type::TempoFader,   DeckA,   false, true },
    {0x0E, Type::TempoFader,   DeckB,   false, true },
    {0x08, Type::Volume,       DeckA,   false, false},
    {0x09, Type::Volume,       DeckB,   false, false},
    {0x0A, Type::Crossfader,   DeckAny, false, false},
    {0x17, Type::MasterVolume, DeckAny, false, false},
    {0x0B, Type::PhonesVolume, DeckAny, false, false},
};

// Relative encoders send two's complement: 1..63 clockwise, 127..65 counter.
int16_t relative(uint8_t v) { return v < 64 ? v : (int16_t)(v - 128); }

Event button(Type t, uint8_t deck, bool pressed)
{
    Event e; e.type = t; e.deck = deck; e.pressed = pressed; return e;
}

} // namespace

int Decoder::decode(uint8_t status, uint8_t d1, uint8_t d2, Event* out, int max)
{
    if (max < 2) return 0;
    const uint8_t kind = status & 0xF0;
    const bool noteOn  = kind == 0x90 && d2 > 0;
    const bool noteOff = kind == 0x80 || (kind == 0x90 && d2 == 0);

    if (noteOn || noteOff) {
        if (d1 == kLoadA || d1 == kLoadB) {
            const uint8_t dk = d1 == kLoadA ? DeckA : DeckB;
            if (noteOn) {
                shiftHeld_[dk] = true;
                shiftConsumed_[dk] = false;
                out[0] = button(Type::Shift, dk, true);
                return 1;
            }
            const bool wasShift = shiftConsumed_[dk];
            shiftHeld_[dk] = false;
            out[0] = button(Type::Shift, dk, false);
            if (wasShift) return 1;
            out[1] = button(Type::Load, dk, true);
            return 2;
        }
        if (d1 == kBack || d1 == kEnter) {
            if (!noteOff) return 0;
            out[0] = button(d1 == kBack ? Type::Back : Type::Load, DeckAny, true);
            return 1;
        }
        for (const NoteMap& m : kNotes) {
            if (m.note != d1) continue;
            if (noteOn && shiftHeld_[m.deck]) shiftConsumed_[m.deck] = true;
            out[0] = button(m.type, m.deck, noteOn);
            return 1;
        }
        return 0;
    }

    if (kind == 0xB0) {
        for (const CcMap& m : kCcs) {
            if (m.cc != d1) continue;
            Event e; e.type = m.type; e.deck = m.deck;
            if (m.relative) {
                e.delta = relative(d2);
                if (m.deck != DeckAny && shiftHeld_[m.deck]) shiftConsumed_[m.deck] = true;
            } else {
                // Tempo faders have a centre detent that reads 63 or 64
                // depending on the unit; both mean exactly 0 %.
                if (m.type == Type::TempoFader && (d2 == 63 || d2 == 64)) {
                    e.position = 0.5f;
                } else {
                    float p = d2 / 127.0f;
                    e.position = m.invert ? 1.0f - p : p;
                }
            }
            out[0] = e;
            return 1;
        }
    }
    return 0;
}

int Decoder::decodeUsbPacket(const uint8_t p[4], Event* out, int max)
{
    const uint8_t cin = p[0] & 0x0F;
    // CIN 0x8 note off, 0x9 note on, 0xB control change — all DJ2Go sends.
    if (cin != 0x8 && cin != 0x9 && cin != 0xB) return 0;
    return decode(p[1], p[2], p[3], out, max);
}

} // namespace dj2go
