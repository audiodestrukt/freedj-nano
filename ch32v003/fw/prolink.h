// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// prolink: Pioneer Pro DJ Link as a player. We announce ourselves, send our status and beats, listen to every
// other player's, find the tempo master, and hand master over both ways. freedj-esp's p4/deck/main/prolink.cpp
// in C: the packet layouts are freedj-3000's (crates/link/src/prodj.rs), checked byte for byte against an
// XDJ-1000MK2 and prolink-cpp's virtual CDJ. Reference: https://djl-analysis.deepsymmetry.org/
//   50000  announce / keep-alive, broadcast every 1.5 s
//   50001  beat packets, broadcast once per beat by each playing player; master handoff, sync and mixer control
//   50002  status packets, unicast to every known device 5 times a second
//
// Pure logic, no OS, no sockets, no libc (the FM-1 module build has none): the caller hands in each datagram
// (pl_rx) and calls pl_tick often, and the packets to send come out through a callback. The same code runs on
// lwIP (Wi-Fi), a USB Ethernet adapter's own small stack, or the host's sockets (sim/prolink_test.c, the
// emulator harness). Single threaded: call everything from one loop. Times are wrap-safe uint32 microseconds.
// IPv4 addresses are uint32s holding the four bytes in wire order (memcpy'd, as lwIP's and BSD's s_addr).
#pragma once
#include <stdint.h>

enum { PL_PORT_ANNOUNCE = 50000, PL_PORT_BEAT = 50001, PL_PORT_STATUS = 50002 };
enum { PL_MAX_PEERS = 8, PL_NAME = 20, PL_STATUS_LEN = 0x124 };

typedef void (*pl_send_fn)(void* ctx, uint32_t ip, uint16_t port, const uint8_t* data, uint32_t n);

// The tempo master as last heard.
typedef struct {
    uint8_t  present;                             // a master is on the network (status or beats seen recently)
    uint8_t  player;
    uint8_t  beating;                             // it sent a beat within the last two beat periods
    uint8_t  beatInBar;                           // 1..4 for that beat
    uint8_t  playing;                             // its status says it plays (0 with no status yet)
    float    bpm;                                 // effective tempo: track BPM x pitch
    uint32_t beatUs;                              // when its last beat packet arrived
} pl_master;

// What we tell the network about ourselves.
typedef struct {
    uint8_t  loaded, playing, emergencyLoop, sync;   // emergencyLoop: play state 0x12, a CDJ whose media was pulled
    float    trackBpm;                            // grid BPM at the playhead, unpitched; 0 = none
    float    pitch;                               // speed multiplier
    uint32_t beat;                                // beat number from the track start (1-based), 0 = none
    uint8_t  beatInBar;                           // 1..4, 0 = none
    uint32_t trackId;                             // the loaded track's rekordbox ID, 0 = not known
    // the next beat as it will be heard, for our beat packets: its number (0 = none), when, its place in the bar
    uint32_t nextBeat, nextBeatUs;
    uint8_t  nextBeatInBar;
} pl_ours;

typedef struct {
    uint8_t  player, seen, haveStatus, haveBeat;
    uint32_t ip, seenUs;                          // last packet of any kind
    // from status packets
    uint32_t statusUs, syncN;
    uint8_t  master, playing, handoffTo;          // handoffTo: the player it hands master to (0xFF none)
    float    trackBpm, pitch;
    // from beat packets
    uint32_t beatUs;
    float    beatBpm;                             // effective
    uint8_t  beatInBar;
} pl_peer;

// Things that happened, for the log (pl_event)
enum {
    PL_EV_NONE, PL_EV_PEER,                       // arg: a new player (its number)
    PL_EV_MASTER_TAKEN, PL_EV_MASTER_HANDED,      // we are master (arg: 1 nobody had it, 2 handed over) / gave it to arg
    PL_EV_YIELDING, PL_EV_YIELDED,                // arg asked us for master, we yield / arg agreed to yield to us
    PL_EV_ASKING, PL_EV_TIMEOUT,                  // asking the master (arg) to hand over / the handoff timed out
    PL_EV_SYNC_CTL, PL_EV_ON_AIR,                 // arg sent sync control / our channel on (1) or off (0) air
    PL_EV_CLAIMING, PL_EV_CLAIMED,                // claiming player number arg / it's ours
    PL_EV_TAKEN, PL_EV_DEFENDING,                 // arg is taken, trying another / defending ours (arg) to a claimant
    PL_EV_RECLAIM,                                // another device holds our number arg: claiming a new one (AUTO)
};
enum { PL_CLAIM_DEFENDED = 4 };

typedef struct {
    uint8_t  player;
    uint8_t  beatsToPeers;                        // our beats unicast to every peer too (Wi-Fi: see pl_begin)
    uint8_t  name[PL_NAME];                       // NUL padded, not terminated: 20 bytes on the wire
    pl_send_fn send;
    void*    ctx;
    pl_peer  peers[PL_MAX_PEERS];
    pl_ours  ours;
    // tempo master
    uint8_t  weMaster, wantMaster, handoffTo, yieldedFrom, sentMaster, asked;
    uint32_t wantSinceUs, lastAskUs, ourSyncN, largestSyncN;
    int8_t   remoteSync, faderStart;
    uint8_t  onAir;
    // the player number claim (pl_claim): 0 held (pl_begin's way), else the stage being sent
    uint8_t  claiming, autoNum, stage, count, tried[6], ntried, defended, reclaim, helloLeft;
    uint32_t claimNextUs, watchUntilUs, ip;
    uint32_t defendedAgainst[PL_CLAIM_DEFENDED], warned[PL_CLAIM_DEFENDED];
    uint8_t  nDefended, nWarned;
    // pacing
    uint8_t  started, pending;
    uint32_t nextAnnounceUs, nextStatusUs, counter, lastBeatSent;
    pl_ours  pend;                                // the beat we wait to send
    uint8_t  ev[8][2], evHead, evTail;
    uint8_t  out[PL_STATUS_LEN];
} pl;

void pl_begin(pl* l, uint8_t player, const char* name, pl_send_fn send, void* ctx);
// On Wi-Fi set l->beatsToPeers: each beat also goes to every peer by unicast, which power save doesn't hold
// back (freedj-3000 does this); receivers drop the twin (any beat from a player within 100 ms of its last).
// Claim a player number as the players do (freedj-3000's claim, Deep Symmetry's analysis): hello x 3, then 3
// stages of 3 packets, 300 ms apart; nothing else is sent until it's ours. autoNum: pick a free one (4, 5, 6,
// 3, 2, 1, after listening 3 s) and move on when it's defended; else claim pl_begin's number and keep it even if
// defended. Once held it's defended against claimants, and AUTO moves if another device announces it (the
// higher address moves). l->player is the number held or being tried.
void pl_claim(pl* l, int autoNum);
int  pl_claimed(const pl* l);                     // the number is ours
// a datagram that came in on one of the three ports, from fromIp
void pl_rx(pl* l, uint16_t port, const uint8_t* d, uint32_t n, uint32_t fromIp, uint32_t nowUs);
// often (packets in come first): our announce, status and beats go out from here. ip 0 = not on a network yet.
// Returns how soon (us) it wants to run again: at most 50 ms, sooner for a beat due.
uint32_t pl_tick(pl* l, uint32_t nowUs, uint32_t ip, uint32_t netmask, const uint8_t mac[6]);
void pl_publish(pl* l, const pl_ours* o);
// the tempo master to follow: 0 if there is none, or we are it
int  pl_get_master(const pl* l, uint32_t nowUs, pl_master* out);
int  pl_peers(const pl* l, uint32_t nowUs);
int  pl_is_master(const pl* l);
// take master now if nobody has it, else ask the master to hand it over (it yields, names us, we take it)
void pl_request_master(pl* l, uint32_t nowUs);
// commands from other devices, taken once (0 none): remote sync 1 on / 2 off; fader start 1 play / 2 stop
int  pl_take_remote_sync(pl* l);
int  pl_take_fader_start(pl* l);
int  pl_on_air(const pl* l);                      // our channel is on air at the mixer
int  pl_event(pl* l, uint8_t* arg);               // the next PL_EV_*, PL_EV_NONE when none

// The packet builders and the pitch field, for tests and other tools
uint32_t pl_build_announce(pl* l, uint8_t* p, uint32_t ip, const uint8_t mac[6]);
uint32_t pl_build_hello(pl* l, uint8_t* p);
uint32_t pl_build_claim(pl* l, uint8_t* p, int stage, uint8_t counter, uint32_t ip, const uint8_t mac[6]);
uint32_t pl_build_defense(pl* l, uint8_t* p, uint8_t number, uint32_t ip);
int      pl_pick_number(const uint8_t* used, int nused, const uint8_t* tried, int ntried);   // AUTO's next, 0 none
uint32_t pl_build_status(pl* l, uint8_t* p, const pl_ours* o, uint32_t counter);
uint32_t pl_build_beat(pl* l, uint8_t* p, const pl_ours* o);
float    pl_pitch_of(const uint8_t* p);
