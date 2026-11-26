// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// prolink: see prolink.h (freedj-esp's p4/deck/main/prolink.cpp in C).
#include "prolink.h"

enum { K_ANNOUNCE = 0x06, K_BEAT = 0x28, K_STATUS = 0x0a };
// Port 50001 control packets (Deep Symmetry's analysis; freedj-3000 for the handoff, tested against an XDJ-1000MK2)
enum { K_FADER_START = 0x02, K_ON_AIR = 0x03, K_MASTER_REQ = 0x26, K_MASTER_RSP = 0x27, K_SYNC_CTL = 0x2a };
enum { SYNC_ON = 0x10, SYNC_OFF = 0x20, BECOME_MASTER = 0x01 };
static const uint8_t kMagic[10] = {'Q', 's', 'p', 't', '1', 'W', 'm', 'J', 'O', 'L'};
#define PITCH_UNITY 0x00100000u                   // the pitch field for +0 %
#define FORGET_US   5000000u                      // a peer silent this long is gone
#define FRESH_US    2000000u                      // a status or beat this recent counts
#define TWIN_US     100000u                       // a beat this soon after the last is its twin (200 BPM = 300 ms)

// A CDJ status packet captured from an XDJ-1000MK2 (firmware 1.44, playing a rekordbox track from USB, tempo
// master), name blanked. We copy it and overwrite the fields we understand, so every byte we don't understand is
// what Pioneer firmware sends. From freedj-3000's status_template.rs, which explains byte 0x1f (must be 0x01, or
// an XDJ aborts master handoffs).
static const uint8_t kStatusTemplate[PL_STATUS_LEN] = {
    0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6D, 0x4A, 0x4F, 0x4C, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x05, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x3F,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x3E,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x31, 0x2E, 0x34, 0x34,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xE4, 0xFF, 0xFA, 0x00, 0x10, 0x00, 0x00,
    0x80, 0x00, 0x31, 0x38, 0x7F, 0xFF, 0xFF, 0xFF, 0x00, 0x10, 0x00, 0x00, 0x00, 0x09, 0x01, 0xFF,
    0x00, 0x00, 0x00, 0x4E, 0x01, 0xFF, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x05, 0x75, 0x1F, 0x03, 0x00, 0x00,
    0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x70, 0x00, 0x00, 0x07, 0x6D, 0x00, 0x1B, 0x24, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

// (freestanding: no libc)
static void zero(void* p, uint32_t n) { uint8_t* b = p; while (n--) *b++ = 0; }
static void copy(void* d, const void* s, uint32_t n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; }
static void fill(void* p, uint8_t v, uint32_t n) { uint8_t* b = p; while (n--) *b++ = v; }
static int same(const void* a, const void* b, uint32_t n)
{
    const uint8_t* x = a; const uint8_t* y = b;
    while (n--) if (*x++ != *y++) return 0;
    return 1;
}
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t ipOf(const uint8_t* p) { uint32_t v; copy(&v, p, 4); return v; }   // (wire order)
static int within(uint32_t now, uint32_t t, uint32_t us) { return now - t < us; }

static void event(pl* l, uint8_t ev, uint8_t arg)
{
    const uint8_t next = (uint8_t)((l->evHead + 1) & 7);
    if (next == l->evTail) return;                // (full: the log loses one)
    l->ev[l->evHead][0] = ev; l->ev[l->evHead][1] = arg;
    l->evHead = next;
}

int pl_event(pl* l, uint8_t* arg)
{
    if (l->evTail == l->evHead) return PL_EV_NONE;
    const int e = l->ev[l->evTail][0];
    if (arg) *arg = l->ev[l->evTail][1];
    l->evTail = (uint8_t)((l->evTail + 1) & 7);
    return e;
}

// The pitch field to a speed multiplier. Senders sometimes flag the top byte, so an absurd value falls back to
// unity rather than poisoning the tempo.
float pl_pitch_of(const uint8_t* p)
{
    const float m = (float)(be32(p) & 0x00FFFFFFu) / (float)PITCH_UNITY;
    return m >= 0.25f && m <= 4.0f ? m : 1.0f;
}

static int alive(const pl_peer* p, uint32_t now) { return p->seen && within(now, p->seenUs, FORGET_US); }

static void takeMaster(pl* l, uint8_t why)
{
    l->weMaster = 1;
    l->wantMaster = 0;
    l->yieldedFrom = 0;
    l->ourSyncN = l->largestSyncN + 1;
    event(l, PL_EV_MASTER_TAKEN, why);
}

static pl_peer* peerFor(pl* l, uint8_t player, uint32_t ip, uint32_t now)
{
    pl_peer* free = 0;
    for (int i = 0; i < PL_MAX_PEERS; i++) {
        pl_peer* p = &l->peers[i];
        if (p->player == player && alive(p, now)) { if (ip) p->ip = ip; p->seenUs = now; return p; }
        if (!free && !alive(p, now)) free = p;
    }
    if (!free) return 0;
    zero(free, sizeof *free);
    free->player = player;
    free->ip = ip;
    free->seen = 1;
    free->seenUs = now;
    free->handoffTo = 0xFF;
    free->pitch = 1.0f;
    event(l, PL_EV_PEER, player);
    return free;
}

static int isLink(const uint8_t* d, uint32_t n) { return n > 0x0b && same(d, kMagic, 10); }

static void onAnnounce(pl* l, const uint8_t* d, uint32_t n, uint32_t now)
{
    if (!isLink(d, n) || d[0x0a] != K_ANNOUNCE || n < 0x30) return;
    const uint8_t player = d[0x24];
    if (player == l->player || player == 0) return;
    peerFor(l, player, ipOf(d + 0x2c), now);
}

static void onBeat(pl* l, const uint8_t* d, uint32_t n, uint32_t fromIp, uint32_t now)
{
    if (!isLink(d, n) || d[0x0a] != K_BEAT || n < 0x60) return;
    const uint8_t player = d[0x21];
    if (player == l->player) return;
    pl_peer* p = peerFor(l, player, fromIp, now);
    if (!p) return;
    // a sender on Wi-Fi sends each beat twice (broadcast and unicast): the first one's time is the beat
    if (p->haveBeat && within(now, p->beatUs, TWIN_US)) return;
    p->haveBeat = 1;
    p->beatUs = now;
    p->beatBpm = (float)be16(d + 0x5a) / 100.0f * pl_pitch_of(d + 0x54);   // track BPM x pitch
    p->beatInBar = d[0x5c] >= 1 && d[0x5c] <= 4 ? d[0x5c] : 1;
}

static void onStatus(pl* l, const uint8_t* d, uint32_t n, uint32_t fromIp, uint32_t now)
{
    if (!isLink(d, n) || d[0x0a] != K_STATUS || n < 0xcc) return;
    const uint8_t player = d[0x21];
    if (player == l->player) return;
    pl_peer* p = peerFor(l, player, fromIp, now);
    if (!p) return;
    const uint8_t f = d[0x89];
    const uint16_t bpm = be16(d + 0x92);
    p->haveStatus = 1;
    p->statusUs = now;
    p->master = (f & 0x20) != 0;
    p->playing = (f & 0x40) != 0;
    // A pre-Nexus player (an original CDJ-2000: 0xd0 bytes) leaves the flags byte at 0 and says PLAY only in its
    // play state: playing, looping, cue play, emergency loop (freedj-3000, against cdj-nxs2-qemu's CDJ-2000 4.33).
    if (n < 0x100) { const uint8_t s = d[0x7b]; p->playing = s == 0x03 || s == 0x04 || s == 0x07 || s == 0x12; }
    p->trackBpm = bpm == 0xffff ? 0.0f : (float)bpm / 100.0f;
    p->pitch = pl_pitch_of(d + 0x8c);
    p->handoffTo = d[0x9f];
    p->syncN = be32(d + 0x84);
    if (p->syncN > l->largestSyncN) l->largestSyncN = p->syncN;
    // The master names us as its successor: take it. (Only after its status says so: taking it on the yield
    // reply made an XDJ abort.)
    if (p->master && p->handoffTo == l->player && !l->weMaster && (l->wantMaster || l->yieldedFrom == player))
        takeMaster(l, 2);
    // the player we granted master to has claimed it: let go
    if (p->master && l->weMaster && l->handoffTo == player) {
        l->weMaster = 0;
        l->handoffTo = 0;
        event(l, PL_EV_MASTER_HANDED, player);
    }
}

// The common header of the small port-50001 control packets
static uint32_t controlHeader(pl* l, uint8_t* p, uint8_t kind, uint16_t payload)
{
    zero(p, 0x24u + payload);
    copy(p, kMagic, 10);
    p[0x0a] = kind;
    copy(p + 0x0b, l->name, PL_NAME);
    p[0x1f] = 0x01;
    p[0x21] = l->player;
    put16(p + 0x22, payload);
    return 0x24u + payload;
}

static void onControl(pl* l, const uint8_t* d, uint32_t n, uint32_t fromIp, uint32_t now)
{
    if (!isLink(d, n) || n < 0x28) return;
    switch (d[0x0a]) {
    case K_MASTER_REQ:                            // a player asks us for master
        if (l->weMaster) {
            const uint8_t from = d[0x27];
            l->handoffTo = from;
            event(l, PL_EV_YIELDING, from);
            uint8_t out[0x2c];
            const uint32_t len = controlHeader(l, out, K_MASTER_RSP, 8);
            put32(out + 0x24, l->player);
            put32(out + 0x28, 1);
            l->send(l->ctx, fromIp, PL_PORT_BEAT, out, len);
        }
        break;
    case K_MASTER_RSP:                            // the master answered our request
        if (n >= 0x2c && d[0x2b] == 1 && l->wantMaster) {
            l->yieldedFrom = d[0x27];
            event(l, PL_EV_YIELDED, d[0x27]);
        }
        break;
    case K_SYNC_CTL:                              // sync on / off, or become master
        // Sent straight to the target player. (freedj reads 0x27 as the target, prolink-cpp writes the sender
        // there: don't depend on it.)
        if (n >= 0x2c) {
            const uint32_t cmd = be32(d + 0x28);
            if (cmd == SYNC_ON) l->remoteSync = 1;
            else if (cmd == SYNC_OFF) l->remoteSync = 2;
            else if (cmd == BECOME_MASTER && !l->weMaster) { l->wantMaster = 1; l->wantSinceUs = now; l->asked = 0; }
            event(l, PL_EV_SYNC_CTL, d[0x21]);
        }
        break;
    case K_ON_AIR:                                // the mixer: which channels are on air
        if (l->player >= 1 && l->player <= 4) {
            const uint8_t air = d[0x24 + l->player - 1] != 0;
            if (air != l->onAir) event(l, PL_EV_ON_AIR, air);
            l->onAir = air;
        }
        break;
    case K_FADER_START:                           // the mixer: 0 start, 1 stop, 2 leave alone
        if (l->player >= 1 && l->player <= 4) {
            const uint8_t c = d[0x24 + l->player - 1];
            if (c == 0 || c == 1) l->faderStart = c == 0 ? 1 : 2;
        }
        break;
    }
}

void pl_rx(pl* l, uint16_t port, const uint8_t* d, uint32_t n, uint32_t fromIp, uint32_t nowUs)
{
    if (port == PL_PORT_BEAT) {
        if (n > 0x0a && d[0x0a] == K_BEAT) onBeat(l, d, n, fromIp, nowUs);
        else onControl(l, d, n, fromIp, nowUs);
    } else if (port == PL_PORT_STATUS) {
        onStatus(l, d, n, fromIp, nowUs);
    } else if (port == PL_PORT_ANNOUNCE) {
        onAnnounce(l, d, n, nowUs);
    }
}

int pl_get_master(const pl* l, uint32_t now, pl_master* out)
{
    zero(out, sizeof *out);
    if (l->weMaster) return 0;                    // we are the reference
    // the player whose status says master; failing that (no status reaching us yet), the only one sending beats
    const pl_peer* m = 0;
    const pl_peer* beater = 0;
    int beaters = 0;
    for (int i = 0; i < PL_MAX_PEERS; i++) {
        const pl_peer* p = &l->peers[i];
        if (!alive(p, now)) continue;
        if (p->master && p->haveStatus && within(now, p->statusUs, FRESH_US)) m = p;
        if (p->haveBeat && within(now, p->beatUs, FRESH_US)) { beaters++; beater = p; }
    }
    if (!m && beaters == 1) m = beater;
    if (!m) return 0;
    out->present = 1;
    out->player = m->player;
    const int fresh = m->haveBeat && m->beatBpm > 0;
    // the status updates the tempo 5x a second, beats once per beat: whichever is newer
    const int statusNewer = m->haveStatus && (!m->haveBeat || (int32_t)(m->statusUs - m->beatUs) > 0);
    if (statusNewer && m->trackBpm > 0) out->bpm = m->trackBpm * m->pitch;
    else out->bpm = fresh ? m->beatBpm : m->trackBpm * m->pitch;
    out->beatUs = m->beatUs;
    out->beatInBar = m->beatInBar;
    out->playing = m->haveStatus && m->playing;
    out->beating = fresh && out->bpm > 0 && (float)(uint32_t)(now - m->beatUs) < 2.0f * 60e6f / out->bpm;
    return 1;
}

int pl_peers(const pl* l, uint32_t now)
{
    int n = 0;
    for (int i = 0; i < PL_MAX_PEERS; i++) n += alive(&l->peers[i], now);
    return n;
}

int pl_is_master(const pl* l) { return l->weMaster; }

void pl_request_master(pl* l, uint32_t now)
{
    if (l->weMaster) return;
    l->wantMaster = 1;
    l->wantSinceUs = now;
    l->yieldedFrom = 0;
    l->asked = 0;
}

int pl_take_remote_sync(pl* l) { const int r = l->remoteSync; l->remoteSync = 0; return r; }
int pl_take_fader_start(pl* l) { const int r = l->faderStart; l->faderStart = 0; return r; }
int pl_on_air(const pl* l) { return l->onAir; }
void pl_publish(pl* l, const pl_ours* o) { l->ours = *o; }

// ── Packets we send ──

uint32_t pl_build_announce(pl* l, uint8_t* p, uint32_t ip, const uint8_t mac[6])
{
    zero(p, 0x36);
    copy(p, kMagic, 10);
    p[0x0a] = K_ANNOUNCE;
    copy(p + 0x0c, l->name, PL_NAME);
    p[0x20] = 0x01;
    p[0x21] = 0x02;
    put16(p + 0x22, 0x36);
    p[0x24] = l->player;
    p[0x25] = 0x01;                               // device type: CDJ
    copy(p + 0x26, mac, 6);
    copy(p + 0x2c, &ip, 4);
    p[0x30] = 0x01;
    p[0x34] = 0x01;
    return 0x36;
}

uint32_t pl_build_status(pl* l, uint8_t* p, const pl_ours* o, uint32_t counter)
{
    copy(p, kStatusTemplate, PL_STATUS_LEN);
    copy(p + 0x0b, l->name, PL_NAME);
    p[0x1f] = 0x01;
    p[0x21] = l->player;
    p[0x24] = l->player;
    // the track's source: our own player's USB, rekordbox-analysed, so peers trust the BPM we report
    p[0x28] = o->loaded ? l->player : 0;
    p[0x29] = o->loaded ? 0x03 : 0;
    p[0x2a] = o->loaded ? 0x01 : 0;
    put32(p + 0x2c, o->loaded ? o->trackId : 0);   // (always: the template's is its capture's track, ID 63)
    // Our USB and SD slots read as empty. The template came from an XDJ with a stick in (0x6f = 0x00, "USB
    // loaded"), so peers listed our USB under LINK and then failed to browse it ("link error"): we answer
    // neither media queries nor the dbserver (TCP 12523).
    p[0x6a] = 0x04; p[0x6b] = 0x04;               // no USB / SD activity
    p[0x6f] = 0x04; p[0x73] = 0x04;               // USB / SD local: no media
    p[0x75] = 0x00;                               // no Link media on our side
    p[0x7b] = !o->loaded ? 0x00 : o->emergencyLoop ? 0x12 : o->playing ? 0x03 : 0x05;
    copy(p + 0x7c, "0.1 ", 4);
    p[0x89] = (uint8_t)(0x84 | (o->playing ? 0x40 : 0) | (l->weMaster ? 0x20 : 0) | (o->sync ? 0x10 : 0) |
                        (l->onAir ? 0x08 : 0));
    p[0x8a] = o->playing ? 0xFF : 0x8D;
    p[0x8b] = o->playing ? 0xFA : 0xFE;
    const uint32_t pitch = (uint32_t)(o->pitch * (float)PITCH_UNITY);
    put32(p + 0x8c, pitch); put32(p + 0x98, pitch); put32(p + 0xc0, pitch); put32(p + 0xc4, pitch);
    const int bpm = o->loaded && o->trackBpm > 0;
    put16(p + 0x90, bpm ? 0x8000 : 0x7FFF);
    put16(p + 0x92, bpm ? (uint16_t)(o->trackBpm * 100.0f + 0.5f) : 0xFFFF);
    p[0x9d] = o->playing ? 0x0D : 0x01;
    p[0x9e] = l->weMaster ? 0x01 : 0x00;
    p[0x9f] = l->handoffTo ? l->handoffTo : 0xFF;
    put32(p + 0xa0, o->beat ? o->beat : 0xFFFFFFFFu);
    put16(p + 0xa4, 0x01FF);
    p[0xa6] = o->beatInBar;
    put32(p + 0xc8, counter);
    put32(p + 0x84, l->ourSyncN);
    return PL_STATUS_LEN;
}

// A beat packet (0x28, 96 bytes), broadcast on each of our beats. The BPM is the track's (unpitched), the pitch
// separate: receivers multiply them (Beat Link's effective tempo). The six countdowns are ms to the next beat,
// the 2nd beat, the next bar, the 4th beat, the 2nd bar and the 8th beat.
uint32_t pl_build_beat(pl* l, uint8_t* p, const pl_ours* o)
{
    zero(p, 0x60);
    copy(p, kMagic, 10);
    p[0x0a] = K_BEAT;
    copy(p + 0x0b, l->name, PL_NAME);
    p[0x1f] = 0x01;
    p[0x21] = l->player;
    put16(p + 0x22, 0x60 - 0x24);
    const float eff = o->trackBpm * o->pitch;
    const float beatMs = 60000.0f / (eff > 1 ? eff : 1);
    const int bib = o->nextBeatInBar >= 1 && o->nextBeatInBar <= 4 ? o->nextBeatInBar : 1;
    const float toBar = (float)(5 - bib);
    const float counts[6] = {1, 2, toBar, 4, toBar + 4, 8};
    for (int i = 0; i < 6; i++) put32(p + 0x24 + 4 * i, (uint32_t)(counts[i] * beatMs + 0.5f));
    fill(p + 0x3c, 0xFF, 24);
    put32(p + 0x54, (uint32_t)(o->pitch * (float)PITCH_UNITY));
    put16(p + 0x5a, (uint16_t)(o->trackBpm * 100.0f + 0.5f));
    p[0x5c] = (uint8_t)bib;
    p[0x5f] = l->player;
    return 0x60;
}

void pl_begin(pl* l, uint8_t player, const char* name, pl_send_fn send, void* ctx)
{
    zero(l, sizeof *l);
    l->player = player;
    for (int i = 0; i < PL_NAME && name[i]; i++) l->name[i] = (uint8_t)name[i];
    l->send = send;
    l->ctx = ctx;
    l->ours.pitch = 1.0f;
}

static void sendBeat(pl* l, uint32_t now, uint32_t bcast, const pl_ours* o)
{
    const uint32_t n = pl_build_beat(l, l->out, o);
    l->send(l->ctx, bcast, PL_PORT_BEAT, l->out, n);
    if (l->beatsToPeers)
        for (int i = 0; i < PL_MAX_PEERS; i++)
            if (alive(&l->peers[i], now) && l->peers[i].ip) l->send(l->ctx, l->peers[i].ip, PL_PORT_BEAT, l->out, n);
}

uint32_t pl_tick(pl* l, uint32_t now, uint32_t ip, uint32_t netmask, const uint8_t mac[6])
{
    uint32_t wait = 50000;
    // (wrap safety: forget what's old here, so a uint32 time that comes round again can't look recent)
    for (int i = 0; i < PL_MAX_PEERS; i++) {
        pl_peer* p = &l->peers[i];
        if (p->seen && !within(now, p->seenUs, FORGET_US)) p->seen = 0;
        if (p->haveStatus && !within(now, p->statusUs, 2 * FRESH_US)) p->haveStatus = 0;
        if (p->haveBeat && !within(now, p->beatUs, 2 * FRESH_US)) p->haveBeat = 0;
    }
    if (!ip) return wait;
    if (!l->started) { l->started = 1; l->nextAnnounceUs = now; l->nextStatusUs = now; }
    uint8_t* out = l->out;
    const uint32_t bcast = ip | ~netmask;
    // our beat: send it when it's heard
    {
        const pl_ours* o = &l->ours;
        if (!o->playing || o->trackBpm <= 0 || !o->nextBeat) {
            l->pending = 0;
            l->lastBeatSent = 0;
        } else {
            // moved backwards (a loop wrapped, a cue, a jump): count again
            if (o->nextBeat < l->lastBeatSent || (l->pending && o->nextBeat < l->pend.nextBeat)) {
                l->lastBeatSent = o->nextBeat - 1;
                l->pending = 0;
            }
            // the audio moved on past the beat we hold: send it now (late) rather than lose it, then take the new one
            if (l->pending && o->nextBeat > l->pend.nextBeat) {
                sendBeat(l, now, bcast, &l->pend);
                l->lastBeatSent = l->pend.nextBeat;
                l->pending = 0;
            }
            if (o->nextBeat > l->lastBeatSent) { l->pend = *o; l->pending = 1; }   // the prediction refreshed
            if (l->pending && (int32_t)(l->pend.nextBeatUs - now) <= 500) {
                sendBeat(l, now, bcast, &l->pend);
                l->lastBeatSent = l->pend.nextBeat;
                l->pending = 0;
            }
            if (l->pending) {
                const int32_t to = (int32_t)(l->pend.nextBeatUs - now);
                if ((uint32_t)to < wait) wait = to < 1000 ? 1000 : (uint32_t)to;
            }
        }
    }
    // asking for master: take it if nobody has it, else ask the master
    if (l->wantMaster && !l->weMaster) {
        uint32_t masterIp = 0;
        uint8_t masterPlayer = 0;
        for (int i = 0; i < PL_MAX_PEERS; i++) {
            const pl_peer* p = &l->peers[i];
            if (alive(p, now) && p->master && p->haveStatus && within(now, p->statusUs, FRESH_US)) {
                masterIp = p->ip; masterPlayer = p->player;
            }
        }
        if (!masterIp) takeMaster(l, 1);
        else if (!within(now, l->wantSinceUs, 5000000u)) { l->wantMaster = 0; event(l, PL_EV_TIMEOUT, masterPlayer); }
        else if (!l->yieldedFrom && (!l->asked || !within(now, l->lastAskUs, 1000000u))) {
            const uint32_t len = controlHeader(l, out, K_MASTER_REQ, 4);
            put32(out + 0x24, l->player);
            l->send(l->ctx, masterIp, PL_PORT_BEAT, out, len);
            l->asked = 1;
            l->lastAskUs = now;
            event(l, PL_EV_ASKING, masterPlayer);
        }
    }
    if ((int32_t)(now - l->nextAnnounceUs) >= 0) {
        l->nextAnnounceUs = now + 1500000u;
        l->send(l->ctx, bcast, PL_PORT_ANNOUNCE, out, pl_build_announce(l, out, ip, mac));
    }
    // status 5x a second, and at once when our master flag changes: an outgoing master waits to see the new one
    // claim it
    if ((int32_t)(now - l->nextStatusUs) >= 0 || l->weMaster != l->sentMaster) {
        l->sentMaster = l->weMaster;
        l->nextStatusUs = now + 200000u;
        const uint32_t n = pl_build_status(l, out, &l->ours, ++l->counter);
        for (int i = 0; i < PL_MAX_PEERS; i++)
            if (alive(&l->peers[i], now) && l->peers[i].ip) l->send(l->ctx, l->peers[i].ip, PL_PORT_STATUS, out, n);
    }
    return wait;
}
