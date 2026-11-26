// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// linksync: the deck's side of Pro DJ Link SYNC, from the master's beats (fw/prolink.h's pl_master). Pure logic,
// no libc, single precision (tiny devices, some without an FPU), wrap-safe uint32 microseconds. freedj-3000's
// PeerPhase and SyncPhase (crates/app/src/prodj.rs) in C, with their tests (sim/linksync_test.c).
//
// ls_peer: the master's beat phase as a phase-locked free run. Beat packets over Wi-Fi arrive late and in
// bursts (an access point holds broadcasts for a power-saving client until its beacon), so it runs at the
// master's tempo on our own clock and each packet only asks for a quarter of its error, bled in as a change of
// pace of at most 25 %: it never runs backwards or jumps. It holds only when the master's status says paused and
// no beat has come for two periods. The beat in the bar turns over with the phase, not the packet.
//
// ls_sync: on each master beat, where our own beat is (frac, 0..1) against the phase offset SYNC holds: correct
// half the error for one beat (at most 3 %, a dead band of 0.01 beat). A jog nudge while synced is kept, as a
// CDJ does (SHIFT GRID then writes it into the grid): measured at the first master beat after the jog settles.
#pragma once
#include <stdint.h>

#define LS_PULL 0.25f                             // of a packet's phase error, asked for as correction
#define LS_SLEW 0.25f                             // the largest change of pace while it bleeds in

typedef struct {
    int32_t  k;                                   // whole beats of the free run; -1 not started
    float    f;                                   // and the fraction, 0..1
    int8_t   barOffset;                           // beat in bar of free-run beat 0 (0-3); -1 unknown
    uint32_t foldedUs;                            // the last packet folded in
    float    pending;                             // correction still to bleed in, beats (signed)
} ls_peer;

void  ls_peer_init(ls_peer* p);
// One frame, dtS since the last: the master's last beat packet (arrived at beatUs, haveBeat 0 = none yet), its
// tempo, its beat in the bar (1-4, 0 unknown), whether its status says it plays. Returns the phase, 0..1, and
// (*shown) the beat in the bar it belongs to, both from the free run.
float ls_peer_advance(ls_peer* p, uint32_t nowUs, uint32_t beatUs, int haveBeat, float bpm, uint8_t bib,
                      int playing, float dtS, uint8_t* shown);
float ls_peer_phase(const ls_peer* p);            // the free run in beats (k + f), for tests

typedef struct { uint8_t pending; } ls_sync;
enum { LS_NONE, LS_HOLD, LS_CORRECT };
// Every pass: 1 = SYNC is off, clear the held offset.
int   ls_sync_tick(ls_sync* s, int sync, int jogging);
// A master beat landed with us frac (0..1) into our own beat. LS_HOLD: hold *out as the offset (beats) from now;
// LS_CORRECT: run at the tempo-matched speed plus *out (a fraction) for one beat.
int   ls_sync_on_master_beat(ls_sync* s, float frac, float offset, int jogging, float* out);
// the error SYNC corrects, beats within +-0.5: frac against the held offset
float ls_phase_err(float frac, float offset);
// the speed (fader) that matches our effective tempo to the master's, within +-range (0.16 = 16 %)
float ls_tempo_match(float masterBpm, float trackBpm, float range);
