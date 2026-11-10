// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The deck: nano-lite plus the transport (CDJ-style PLAY and CUE), the controls (EC11,
// buttons, pitch pot, the DJ2Go over the bridge), the track list and the screen.
// On the chip: app_init() once, then app_step() forever.
//
//   turn             jog: nudge while playing; while paused, scrub (play what's under it,
//                    either way, at the speed it turns, up to 4x)
//   SHIFT + turn     the track list; push loads (BACK or 8 s idle closes it)
//   push             keylock on / off;  SHIFT + push: tempo range 6 / 10 / 16 / 50 %
//   PLAY, CUE        CDJ: CUE while playing goes back to the cue point and pauses; while
//                    paused it sets the cue here and previews while held
//   pot              tempo
// Console keys on the bridge: p c k (play, cue, keylock) + - 0 (tempo) r (range)
// n b (next / previous track) j J (jog) l (list).
#include <stdint.h>
#include "lite.h"
#include "tracks.h"

#define APP_NONE 0xFFFF
enum { ST_STOP, ST_PLAY, ST_CUE };

typedef struct {
    uint16_t track, count, sel;   // sel: highlighted in the list
    uint8_t  state, browsing, keylock, err, range;
    int16_t  tempo;               // 1/10 %
    uint32_t frames, cue, bpmX100, firstBeat, peaksOff;   // peaksOff 0: no waveform
    char     name[TRK_NAME];
    uint8_t  ov[64];              // overview, 128 columns of 0..15
} app_view;

extern app_view  g_app;
extern lite_deck g_deck;

void app_init(void);
void app_step(void);
