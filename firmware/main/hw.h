// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The firmware's hardware modules, one small interface each. Everything here
// runs in the control task except audio::write().

#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "nano/library.h"
#include "nano/ui.h"

namespace audio {   // audio.cpp: I2S out to a PCM5102A (no MCLK: tie its SCK to GND)
bool begin(uint32_t rate);
void setRate(uint32_t rate);                 // audio task, between blocks
void write(const int16_t* stereo, int frames);   // blocks on the DMA
}

namespace tracks {  // tracks.cpp: the `tracks` flash partition, memory-mapped
bool begin(nano::Library& lib);              // false: no partition or no pack in it
}

namespace inputs {  // inputs.cpp: EC11 on PCNT, buttons (GPIO or ADC ladder), pitch pot
enum Button : uint8_t { Play, Cue, Key, List, Push, kButtons };
struct State {
    int  encoder = 0;            // quadrature counts since the last poll (4 per EC11 detent)
    bool down[kButtons] = {};
    bool pressed[kButtons] = {}; // went down this poll
    bool released[kButtons] = {};
    int  pot = -1;               // 0..1000, or -1 when unchanged / absent
};
void begin();
void poll(State& s);             // every ~5 ms
}

namespace midi_uart {  // midi_uart.cpp: DIN/TRS MIDI in
void begin();
// Reads whatever arrived; returns complete messages via the callback.
void poll(void (*onMsg)(uint8_t status, uint8_t d1, uint8_t d2));
}

namespace display {  // display_u8g2.cpp (mono) or display_tft.cpp (TFT + touch)
bool begin(i2c_master_bus_handle_t bus);
int  width();
int  height();
void present(const ui::Model& m);
bool touch(bool& down, int& x, int& y);      // false: no touch panel
void tune(char key);                         // TFT panel voltages from the console (i, v, V, f)
uint32_t sentBytes();                        // bytes sent to the panel since the last call
}
