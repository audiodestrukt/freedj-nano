// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// prolink_test: fw/prolink.c on the host. A real XDJ-1000MK2's status and beat (sim/prolink_xdj.h) make it follow
// that player as the tempo master; our announce, status and beats come out as freedj-esp's did; master handoff
// both ways, the mixer's and the players' commands, peers forgotten, the clock wrapping. Then freedj-3000's link
// tests (crates/link/src/prodj.rs, crates/app/src/prodj.rs) on its captured packets (sim/prolink_f3k.h).   make prolinktest
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "prolink.h"
#include "prolink_xdj.h"
#include "prolink_f3k.h"

static int s_fail, s_n;
#define CHECK(c, ...) do { s_n++; if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// what pl sent
typedef struct { uint32_t ip; uint16_t port; uint32_t n; uint8_t d[PL_STATUS_LEN]; } sent;
static sent s_out[64];
static int s_nout;
static void cap(void* ctx, uint32_t ip, uint16_t port, const uint8_t* d, uint32_t n)
{
    (void)ctx;
    if (s_nout < 64) { s_out[s_nout].ip = ip; s_out[s_nout].port = port; s_out[s_nout].n = n; memcpy(s_out[s_nout].d, d, n); s_nout++; }
}
static const sent* find(uint16_t port, uint8_t kind)
{
    for (int i = 0; i < s_nout; i++) if (s_out[i].port == port && s_out[i].d[0x0a] == kind) return &s_out[i];
    return 0;
}
static int count(uint16_t port, uint8_t kind)
{
    int c = 0;
    for (int i = 0; i < s_nout; i++) c += s_out[i].port == port && s_out[i].d[0x0a] == kind;
    return c;
}
static uint32_t ip4(int a, int b, int c, int d) { const uint8_t q[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d}; uint32_t v; memcpy(&v, q, 4); return v; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

static const uint8_t kMac[6] = {0x02, 0xf1, 0x00, 0x00, 0x00, 0x03};
static uint32_t OUR, PEER, MASK;

static void setup(pl* l, uint32_t t0)
{
    pl_begin(l, 3, "FM-1", cap, 0);
    s_nout = 0;
    (void)t0;
}
// the XDJ's status, with its flags / handoff / player changed as asked
static void xdjStatus(pl* l, uint32_t now, uint8_t flags, uint8_t handoff)
{
    uint8_t s[sizeof kXdjStatus];
    memcpy(s, kXdjStatus, sizeof s);
    s[0x89] = flags;
    s[0x9f] = handoff;
    pl_rx(l, PL_PORT_STATUS, s, sizeof s, PEER, now);
}
static void control(pl* l, uint32_t now, uint8_t kind, uint8_t at27, uint32_t at28, const uint8_t* payload4)
{
    uint8_t c[0x2c] = {'Q', 's', 'p', 't', '1', 'W', 'm', 'J', 'O', 'L'};
    c[0x0a] = kind;
    c[0x21] = 1;
    c[0x27] = at27;
    c[0x28] = (uint8_t)(at28 >> 24); c[0x29] = (uint8_t)(at28 >> 16); c[0x2a] = (uint8_t)(at28 >> 8); c[0x2b] = (uint8_t)at28;
    if (payload4) memcpy(c + 0x24, payload4, 4);
    pl_rx(l, PL_PORT_BEAT, c, sizeof c, PEER, now);
}

static void follows_a_real_xdj(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    pl_master m;
    CHECK(!pl_get_master(&l, t0, &m), "no master yet");
    xdjStatus(&l, t0, 0xe4, 0xff);
    CHECK(pl_get_master(&l, t0 + 1000, &m) && m.player == 1, "the XDJ is master (player %d)", m.player);
    CHECK(fabsf(m.bpm - 126.05f) < 0.01f, "at 126.05 BPM (%.3f)", m.bpm);
    CHECK(!m.beating, "no beats yet");
    pl_rx(&l, PL_PORT_BEAT, kXdjBeat, sizeof kXdjBeat, PEER, t0 + 2000);
    CHECK(pl_get_master(&l, t0 + 3000, &m) && m.beating && m.beatUs == t0 + 2000 && m.beatInBar == 1, "beating, its beat's time");
    CHECK(fabsf(m.bpm - 126.05f) < 0.01f, "the beat's tempo (%.3f)", m.bpm);
    // two beat periods (952 ms) without a beat: still master, not beating; its status keeps it present
    xdjStatus(&l, t0 + 1100000, 0xe4, 0xff);
    CHECK(pl_get_master(&l, t0 + 1100000, &m) && !m.beating, "not beating after 1.1 s");
    uint8_t arg;
    CHECK(pl_event(&l, &arg) == PL_EV_PEER && arg == 1, "the peer logged");
    CHECK(pl_peers(&l, t0 + 1100000) == 1, "one peer");
    // 5 s silent: forgotten
    pl_tick(&l, t0 + 6200000, OUR, MASK, kMac);
    CHECK(!pl_get_master(&l, t0 + 6200000, &m) && pl_peers(&l, t0 + 6200000) == 0, "forgotten after 5 s");
}

static void only_beats(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    pl_master m;
    pl_rx(&l, PL_PORT_BEAT, kXdjBeat, sizeof kXdjBeat, PEER, t0);
    CHECK(pl_get_master(&l, t0 + 10, &m) && m.player == 1 && m.beating, "the only player beating is followed");
    uint8_t b2[sizeof kXdjBeat];
    memcpy(b2, kXdjBeat, sizeof b2);
    b2[0x21] = 2;
    pl_rx(&l, PL_PORT_BEAT, b2, sizeof b2, ip4(169, 254, 0, 9), t0 + 20);
    CHECK(!pl_get_master(&l, t0 + 30, &m), "two beating, no status: no master");
    // a pitch field with a flagged top byte, or absurd, reads as unity
    uint8_t p[4] = {0x80, 0x10, 0x00, 0x00};
    CHECK(fabsf(pl_pitch_of(p) - 1.0f) < 1e-6f, "flagged top byte ignored");
    uint8_t q[4] = {0x00, 0xF0, 0x00, 0x00};
    CHECK(fabsf(pl_pitch_of(q) - 1.0f) < 1e-6f, "x15 is absurd: unity");
    uint8_t r[4] = {0x00, 0x10, 0x51, 0xEC};          // +2 %
    CHECK(fabsf(pl_pitch_of(r) - 1.02f) < 1e-4f, "+2 %% (%.5f)", pl_pitch_of(r));
}

static void announces_and_reports(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    pl_tick(&l, t0, 0, MASK, kMac);
    CHECK(s_nout == 0, "nothing sent off the network");
    pl_tick(&l, t0, OUR, MASK, kMac);
    const sent* a = find(PL_PORT_ANNOUNCE, 0x06);
    CHECK(a && a->n == 0x36 && a->ip == ip4(169, 254, 255, 255), "an announce broadcast");
    if (a) {
        CHECK(!memcmp(a->d, "Qspt1WmJOL", 10) && !memcmp(a->d + 0x0c, "FM-1", 4) && a->d[0x10] == 0, "magic, the name NUL padded");
        CHECK(a->d[0x24] == 3 && a->d[0x25] == 1 && !memcmp(a->d + 0x26, kMac, 6), "player 3, a CDJ, our MAC");
        CHECK(!memcmp(a->d + 0x2c, &OUR, 4) && be16(a->d + 0x22) == 0x36, "our address, the length");
    }
    CHECK(count(PL_PORT_STATUS, 0x0a) == 0, "no status: nobody to send it to");
    // the next announce in 1.5 s, not before
    s_nout = 0;
    pl_tick(&l, t0 + 1499000, OUR, MASK, kMac);
    CHECK(!find(PL_PORT_ANNOUNCE, 0x06), "no announce before 1.5 s");
    pl_tick(&l, t0 + 1500000, OUR, MASK, kMac);
    CHECK(find(PL_PORT_ANNOUNCE, 0x06) != 0, "announce at 1.5 s");
    // a peer: our status to it, 5x a second
    uint8_t ann[0x36] = {0};
    pl_build_announce(&l, ann, PEER, kMac);
    ann[0x24] = 1;
    pl_rx(&l, PL_PORT_ANNOUNCE, ann, sizeof ann, PEER, t0 + 1500100);
    pl_ours o = {.loaded = 1, .playing = 1, .trackBpm = 124.0f, .pitch = 1.02f, .beat = 17, .beatInBar = 1, .trackId = 41};
    pl_publish(&l, &o);
    s_nout = 0;
    for (uint32_t t = t0 + 1600000; t < t0 + 2600000; t += 10000) pl_tick(&l, t, OUR, MASK, kMac);
    CHECK(count(PL_PORT_STATUS, 0x0a) == 5, "5 statuses in a second (%d)", count(PL_PORT_STATUS, 0x0a));
    const sent* s = find(PL_PORT_STATUS, 0x0a);
    CHECK(s && s->ip == PEER && s->n == PL_STATUS_LEN, "unicast to the peer, 0x124 bytes");
    if (s) {
        CHECK(s->d[0x21] == 3 && s->d[0x24] == 3 && s->d[0x1f] == 1, "player 3, byte 0x1f = 1");
        CHECK(s->d[0x89] == (0x84 | 0x40), "flags: playing, not master (0x%02x)", s->d[0x89]);
        CHECK(be16(s->d + 0x92) == 12400 && be16(s->d + 0x90) == 0x8000, "124.00 BPM");
        CHECK(be32(s->d + 0x8c) == (uint32_t)(1.02f * 0x100000) && be32(s->d + 0xc4) == be32(s->d + 0x8c), "the pitch, four times");
        CHECK(be32(s->d + 0xa0) == 17 && s->d[0xa6] == 1 && s->d[0x7b] == 0x03, "beat 17, 1 of the bar, play state playing");
        CHECK(be32(s->d + 0x2c) == 41 && s->d[0x28] == 3 && s->d[0x29] == 3, "rekordbox ID 41 from our USB");
        // freedj-3000's "no leftover template ID": no ID of ours, none in the status (the template's is 63)
        pl_ours none = o;
        none.trackId = 0;
        uint8_t q[PL_STATUS_LEN];
        pl_build_status(&l, q, &none, 1);
        CHECK(be32(q + 0x2c) == 0, "no track ID: 0, not the template's 63 (%u)", be32(q + 0x2c));
        none.loaded = 0; none.trackId = 41;
        pl_build_status(&l, q, &none, 1);
        CHECK(be32(q + 0x2c) == 0 && q[0x28] == 0, "nothing loaded: no ID, no source");
        CHECK(s->d[0x6f] == 4 && s->d[0x73] == 4 && s->d[0x75] == 0, "our media slots empty");
        const sent* s2 = 0;
        for (int i = s_nout - 1; i >= 0; i--) if (s_out[i].port == PL_PORT_STATUS) { s2 = &s_out[i]; break; }
        CHECK(s2 && be32(s2->d + 0xc8) == be32(s->d + 0xc8) + 4, "the packet counter counts");
    }
    // unchanged bytes are the XDJ template's: its firmware string
    CHECK(s && !memcmp(s->d + 0x7c, "0.1 ", 4), "our firmware string");
}

static void our_beats(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    pl_tick(&l, t0, OUR, MASK, kMac);
    pl_ours o = {.loaded = 1, .playing = 1, .trackBpm = 120.0f, .pitch = 1.0f, .nextBeat = 5, .nextBeatUs = t0 + 100000,
                 .nextBeatInBar = 1};
    pl_publish(&l, &o);
    s_nout = 0;
    uint32_t w = pl_tick(&l, t0 + 10000, OUR, MASK, kMac);
    CHECK(!find(PL_PORT_BEAT, 0x28), "not before the beat");
    CHECK(w == 50000, "wakes in 50 ms (%u)", w);
    w = pl_tick(&l, t0 + 80000, OUR, MASK, kMac);
    CHECK(w == 20000, "wakes for the beat in 20 ms (%u)", w);
    pl_tick(&l, t0 + 99600, OUR, MASK, kMac);
    const sent* b = find(PL_PORT_BEAT, 0x28);
    CHECK(b && b->n == 0x60 && b->ip == ip4(169, 254, 255, 255), "the beat broadcast, 0.4 ms early");
    if (b) {
        CHECK(be32(b->d + 0x24) == 500 && be32(b->d + 0x28) == 1000 && be32(b->d + 0x2c) == 2000, "500 ms to the next beat, 2 s to the bar");
        CHECK(be32(b->d + 0x30) == 2000 && be32(b->d + 0x34) == 4000 && be32(b->d + 0x38) == 4000, "4th beat, 2nd bar, 8th beat");
        CHECK(be16(b->d + 0x5a) == 12000 && be32(b->d + 0x54) == 0x100000 && b->d[0x5c] == 1 && b->d[0x5f] == 3, "120 BPM, unity, beat 1, player 3");
        CHECK(b->d[0x3c] == 0xff && b->d[0x53] == 0xff, "the 24 bytes of 0xff");
        // it parses as a beat: a second pl hears us
        static pl l2;
        pl_begin(&l2, 1, "OTHER", cap, 0);
        pl_rx(&l2, PL_PORT_BEAT, b->d, b->n, OUR, t0 + 99700);
        pl_master m;
        CHECK(pl_get_master(&l2, t0 + 99800, &m) && m.player == 3 && fabsf(m.bpm - 120.0f) < 0.01f, "a peer follows our beats");
    }
    CHECK(count(PL_PORT_BEAT, 0x28) == 1, "once");
    {   // the BPM rounded to the hundredth, in the beat and in the status
        pl_ours r = o;
        r.trackBpm = 123.456f;
        uint8_t pk[PL_STATUS_LEN];
        pl_build_beat(&l, pk, &r);
        CHECK(be16(pk + 0x5a) == 12346, "the beat's 123.456 BPM rounds to 12346 (%u)", be16(pk + 0x5a));
        pl_build_status(&l, pk, &r, 1);
        CHECK(be16(pk + 0x92) == 12346, "the status's too (%u)", be16(pk + 0x92));
    }
    s_nout = 0;
    pl_tick(&l, t0 + 105000, OUR, MASK, kMac);
    CHECK(!find(PL_PORT_BEAT, 0x28), "not twice");
    // a late wake: the audio is already at beat 7 (6 was never sent): 6 goes out late, then 7 on time
    o.nextBeat = 6; o.nextBeatUs = t0 + 600000; o.nextBeatInBar = 2;
    pl_publish(&l, &o);
    pl_tick(&l, t0 + 300000, OUR, MASK, kMac);
    o.nextBeat = 7; o.nextBeatUs = t0 + 1100000; o.nextBeatInBar = 3;
    pl_publish(&l, &o);
    s_nout = 0;
    pl_tick(&l, t0 + 640000, OUR, MASK, kMac);
    CHECK(count(PL_PORT_BEAT, 0x28) == 1 && find(PL_PORT_BEAT, 0x28)->d[0x5c] == 2, "beat 6 late, not lost");
    pl_tick(&l, t0 + 1100000, OUR, MASK, kMac);
    CHECK(count(PL_PORT_BEAT, 0x28) == 2 && s_out[s_nout - 1].d[0x5c] == 3, "then beat 7");
    if (s_nout) CHECK(be32(s_out[s_nout - 1].d + 0x2c) == 1000, "beat 3 of the bar: the bar in 2 beats");
    // a cue back to beat 1: counted afresh
    o.nextBeat = 1; o.nextBeatUs = t0 + 1300000; o.nextBeatInBar = 1;
    pl_publish(&l, &o);
    s_nout = 0;
    pl_tick(&l, t0 + 1200000, OUR, MASK, kMac);
    pl_tick(&l, t0 + 1300000, OUR, MASK, kMac);
    CHECK(count(PL_PORT_BEAT, 0x28) == 1, "after a jump back, beat 1 goes out");
    // stopped: no beats
    o.playing = 0; o.nextBeat = 2; o.nextBeatUs = t0 + 1800000;
    pl_publish(&l, &o);
    s_nout = 0;
    pl_tick(&l, t0 + 1800000, OUR, MASK, kMac);
    CHECK(!find(PL_PORT_BEAT, 0x28), "stopped: none");
}

static void master_nobody_has_it(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    xdjStatus(&l, t0, 0xc4, 0xff);                     // playing, not master
    pl_request_master(&l, t0);
    pl_tick(&l, t0 + 1000, OUR, MASK, kMac);
    CHECK(pl_is_master(&l), "taken: nobody had it");
    const sent* s = find(PL_PORT_STATUS, 0x0a);
    CHECK(s && (s->d[0x89] & 0x20) && s->d[0x9e] == 1 && be32(s->d + 0x84) == 2, "our status: master, sync counter 2 (the XDJ's 1 + 1)");
    pl_master m;
    CHECK(!pl_get_master(&l, t0 + 2000, &m), "we are the reference: nobody to follow");
}

static void master_handed_to_us(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    xdjStatus(&l, t0, 0xe4, 0xff);                     // the XDJ is master
    pl_request_master(&l, t0);
    pl_tick(&l, t0 + 1000, OUR, MASK, kMac);
    const sent* r = find(PL_PORT_BEAT, 0x26);
    CHECK(!pl_is_master(&l) && r && r->ip == PEER && r->n == 0x28 && be32(r->d + 0x24) == 3, "asked the XDJ (0x26 to it, player 3)");
    s_nout = 0;
    pl_tick(&l, t0 + 500000, OUR, MASK, kMac);
    CHECK(!find(PL_PORT_BEAT, 0x26), "not again within a second");
    xdjStatus(&l, t0 + 900000, 0xe4, 0xff);
    pl_tick(&l, t0 + 1001000, OUR, MASK, kMac);
    CHECK(find(PL_PORT_BEAT, 0x26) != 0, "asked again after a second");
    xdjStatus(&l, t0 + 1050000, 0xc4, 3);              // (a status naming us without the master flag: not yet)
    CHECK(!pl_is_master(&l), "a non-master naming us isn't a handover");
    xdjStatus(&l, t0 + 1060000, 0xe4, 0xff);
    control(&l, t0 + 1100000, 0x27, 1, 1, 0);          // it yields
    CHECK(!pl_is_master(&l), "not on the yield alone (an XDJ aborts)");
    xdjStatus(&l, t0 + 1200000, 0xe4, 3);              // its status names us
    CHECK(pl_is_master(&l), "taken when its status names us");
    s_nout = 0;
    pl_tick(&l, t0 + 1201000, OUR, MASK, kMac);
    const sent* s = find(PL_PORT_STATUS, 0x0a);
    CHECK(s && (s->d[0x89] & 0x20), "our status says master at once");
    int e, got = 0;
    uint8_t arg;
    while ((e = pl_event(&l, &arg)) != PL_EV_NONE) if (e == PL_EV_MASTER_TAKEN && arg == 2) got = 1;
    CHECK(got, "logged: handed over");
}

static void master_handoff_times_out(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    pl_request_master(&l, t0);
    for (uint32_t t = 0; t <= 5200000; t += 100000) { xdjStatus(&l, t0 + t, 0xe4, 0xff); pl_tick(&l, t0 + t + 1, OUR, MASK, kMac); }
    CHECK(!pl_is_master(&l) && !l.wantMaster, "gave up after 5 s");
    CHECK(count(PL_PORT_BEAT, 0x26) >= 5 && count(PL_PORT_BEAT, 0x26) <= 6, "asked once a second (%d)", count(PL_PORT_BEAT, 0x26));
}

static void master_yielded_by_us(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    pl_request_master(&l, t0);
    pl_tick(&l, t0, OUR, MASK, kMac);
    CHECK(pl_is_master(&l), "master");
    xdjStatus(&l, t0 + 1000, 0xc4, 0xff);
    s_nout = 0;
    const uint8_t from[4] = {0, 0, 0, 1};
    control(&l, t0 + 2000, 0x26, 1, 0, from);          // the XDJ (player 1) asks
    const sent* r = find(PL_PORT_BEAT, 0x27);
    CHECK(r && r->ip == PEER && r->n == 0x2c && r->d[0x2b] == 1 && be32(r->d + 0x24) == 3, "we yield (0x27 to it)");
    s_nout = 0;
    pl_tick(&l, t0 + 200000, OUR, MASK, kMac);
    const sent* s = find(PL_PORT_STATUS, 0x0a);
    CHECK(s && s->d[0x9f] == 1 && (s->d[0x89] & 0x20), "our status names player 1, still master");
    xdjStatus(&l, t0 + 300000, 0xe4, 0xff);           // it claims
    CHECK(!pl_is_master(&l), "let go when it claims");
    pl_master m;
    CHECK(pl_get_master(&l, t0 + 300001, &m) && m.player == 1, "and follow it");
}

static void commands(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    control(&l, t0, 0x2a, 3, 0x10, 0);
    CHECK(pl_take_remote_sync(&l) == 1 && pl_take_remote_sync(&l) == 0, "sync on, taken once");
    control(&l, t0, 0x2a, 3, 0x20, 0);
    CHECK(pl_take_remote_sync(&l) == 2, "sync off");
    control(&l, t0, 0x2a, 3, 0x01, 0);
    pl_tick(&l, t0 + 10, OUR, MASK, kMac);
    CHECK(pl_is_master(&l), "become master: nobody had it, so taken");
    const uint8_t air[4] = {1, 0, 1, 0};               // channels 1 and 3 on air: ours
    control(&l, t0, 0x03, 0, 0, air);
    CHECK(pl_on_air(&l), "on air");
    const uint8_t start[4] = {2, 2, 0, 2};             // channel 3: start
    control(&l, t0, 0x02, 0, 0, start);
    CHECK(pl_take_fader_start(&l) == 1, "fader start: play");
    const uint8_t stop[4] = {2, 2, 1, 2};
    control(&l, t0, 0x02, 0, 0, stop);
    CHECK(pl_take_fader_start(&l) == 2, "fader start: stop");
    const uint8_t leave[4] = {0, 0, 2, 0};
    control(&l, t0, 0x02, 0, 0, leave);
    CHECK(pl_take_fader_start(&l) == 0, "2: left alone");
    // junk and our own echo ignored
    uint8_t junk[0x40] = {0};
    pl_rx(&l, PL_PORT_STATUS, junk, sizeof junk, PEER, t0);
    uint8_t echo[sizeof kXdjStatus];
    memcpy(echo, kXdjStatus, sizeof echo);
    echo[0x21] = 3;
    pl_rx(&l, PL_PORT_STATUS, echo, sizeof echo, OUR, t0);
    CHECK(pl_peers(&l, t0) == 0, "junk and our own echo: no peers");
    pl_rx(&l, PL_PORT_STATUS, kXdjStatus, 0x80, PEER, t0);
    CHECK(pl_peers(&l, t0) == 0, "a short status ignored");
}

// ── freedj-3000's tests, on its captures ──

static void f3k_captured_beats(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    pl_master m;
    // parses_captured_virtual_cdj_beat: player 5, 128 BPM, beat 2
    pl_rx(&l, PL_PORT_BEAT, kVcdjBeat, sizeof kVcdjBeat, ip4(10, 0, 0, 5), t0);
    CHECK(pl_get_master(&l, t0 + 1, &m) && m.player == 5 && fabsf(m.bpm - 128.0f) < 1e-3f && m.beatInBar == 2,
          "the virtual CDJ: player 5, 128 BPM, beat 2 (%d, %.3f, %d)", m.player, m.bpm, m.beatInBar);
    CHECK(be32(kVcdjBeat + 0x24) == 468, "its next beat in 468 ms");
    // parses_real_xdj_beat: player 1, 126.00, beat 2, unity
    static pl k;
    setup(&k, t0);
    pl_rx(&k, PL_PORT_BEAT, kXdjBeat2, sizeof kXdjBeat2, PEER, t0);
    CHECK(pl_get_master(&k, t0 + 1, &m) && m.player == 1 && fabsf(m.bpm - 126.0f) < 1e-3f && m.beatInBar == 2,
          "the XDJ's beat: 126.00, beat 2");
    CHECK(!m.playing, "no status yet: not known to be playing");
}

static void f3k_idle_status(uint32_t t0)
{
    // parses_captured_xdj_status: idle, no track, not master: present, nothing to follow
    static pl l;
    setup(&l, t0);
    pl_rx(&l, PL_PORT_STATUS, kXdjIdleStatus, sizeof kXdjIdleStatus, PEER, t0);
    pl_master m;
    CHECK(pl_peers(&l, t0) == 1 && !pl_get_master(&l, t0 + 1, &m), "an idle XDJ: a peer, not a master");
    CHECK(l.peers[0].trackBpm == 0.0f && !l.peers[0].playing && l.peers[0].handoffTo == 0xff && l.peers[0].syncN == 1,
          "no BPM, not playing, no handoff, sync counter 1");
    CHECK(be32(kXdjIdleStatus + 0xc8) == 268 && !memcmp(kXdjIdleStatus + 0x7c, "1.44", 4), "(counter 268, firmware 1.44)");
}

static void f3k_pre_nexus(uint32_t t0)
{
    // a_pre_nexus_cdj_2000_playing_is_heard_as_playing: 0xd0 bytes, flags 0, play state 3
    static pl l;
    setup(&l, t0);
    pl_rx(&l, PL_PORT_STATUS, kCdj2000Status, sizeof kCdj2000Status, PEER, t0);
    CHECK(pl_peers(&l, t0) == 1 && l.peers[0].playing, "an original CDJ-2000 plays (from its play state)");
    CHECK(!l.peers[0].master && fabsf(l.peers[0].trackBpm - 126.05f) < 1e-3f, "not master, 126.05 BPM");
    // it can't be master (no flags): its beats alone make it the one to follow
    uint8_t b[sizeof kXdjBeat];
    memcpy(b, kXdjBeat, sizeof b);
    pl_rx(&l, PL_PORT_BEAT, b, sizeof b, PEER, t0 + 10);
    pl_master m;
    CHECK(pl_get_master(&l, t0 + 20, &m) && m.player == 1 && m.playing, "followed by its beats, playing");
    // a Nexus-size status with flags 0 and play state 3 is NOT playing: the flags count there
    uint8_t s[sizeof kXdjIdleStatus];
    memcpy(s, kXdjIdleStatus, sizeof s);
    s[0x7b] = 0x03;
    static pl k;
    setup(&k, t0);
    pl_rx(&k, PL_PORT_STATUS, s, sizeof s, PEER, t0);
    CHECK(!k.peers[0].playing, "a Nexus status's flags decide");
}

static void f3k_beats_round_trip(uint32_t t0)
{
    static pl l;
    setup(&l, t0);
    // beat_round_trips: heard 130 BPM at +2 %: the field the track's 127.45, the next beat 462 ms
    pl_ours o = {.loaded = 1, .playing = 1, .trackBpm = 130.0f / 1.02f, .pitch = 1.02f, .nextBeatInBar = 3};
    uint8_t b[0x60];
    CHECK(pl_build_beat(&l, b, &o) == 0x60, "96 bytes");
    CHECK(be16(b + 0x5a) == 12745 && be32(b + 0x24) == 462 && b[0x5c] == 3, "127.45 BPM, 462 ms, beat 3 (%u, %u)", be16(b + 0x5a), be32(b + 0x24));
    CHECK(fabsf(pl_pitch_of(b + 0x54) - 1.02f) < 1e-4f, "the pitch, +2 %%");
    // beat_bpm_field_is_the_track_tempo_like_a_cdj: 126 BPM at -4.10 % says 126.00 (not 120.83) and the pitch
    o.trackBpm = 126.0f; o.pitch = 1.0f - 0.0410f; o.nextBeatInBar = 1;
    pl_build_beat(&l, b, &o);
    CHECK(be16(b + 0x5a) == 12600, "the track's 126.00, not the heard tempo (%u)", be16(b + 0x5a));
    static pl k;
    pl_begin(&k, 1, "OTHER", cap, 0);                 // (another player: our own number is our echo)
    pl_rx(&k, PL_PORT_BEAT, b, sizeof b, OUR, t0);
    pl_master m;
    CHECK(pl_get_master(&k, t0 + 1, &m) && fabsf(m.bpm - 120.83f) < 0.01f, "heard as 120.83 (%.3f), not 115.88", m.bpm);
}

static void f3k_twins(uint32_t t0)
{
    // twin_beats_within_100ms_are_dropped_and_real_ones_kept
    static pl l;
    setup(&l, t0);
    pl_master m;
    pl_rx(&l, PL_PORT_BEAT, kXdjBeat, sizeof kXdjBeat, PEER, t0);
    pl_rx(&l, PL_PORT_BEAT, kXdjBeat2, sizeof kXdjBeat2, PEER, t0 + 5000);        // the unicast copy
    CHECK(pl_get_master(&l, t0 + 6000, &m) && m.beatUs == t0 && m.beatInBar == 1, "the twin 5 ms later dropped");
    pl_rx(&l, PL_PORT_BEAT, kXdjBeat2, sizeof kXdjBeat2, PEER, t0 + 300000);      // the next beat (200 BPM)
    CHECK(pl_get_master(&l, t0 + 300001, &m) && m.beatUs == t0 + 300000 && m.beatInBar == 2, "300 ms later: kept");
    static pl k;                                                                  // another player isn't a twin
    setup(&k, t0);
    pl_rx(&k, PL_PORT_BEAT, kXdjBeat, sizeof kXdjBeat, PEER, t0);
    pl_rx(&k, PL_PORT_BEAT, kVcdjBeat, sizeof kVcdjBeat, ip4(10, 0, 0, 5), t0 + 5000);
    CHECK(k.peers[1].haveBeat && k.peers[1].beatUs == t0 + 5000, "another player 5 ms later: kept");
    // and we send our own twin when asked: broadcast + one unicast per peer
    static pl w;
    setup(&w, t0);
    w.beatsToPeers = 1;
    pl_rx(&w, PL_PORT_STATUS, kXdjIdleStatus, sizeof kXdjIdleStatus, PEER, t0);
    pl_ours o = {.loaded = 1, .playing = 1, .trackBpm = 120.0f, .pitch = 1.0f, .nextBeat = 1, .nextBeatUs = t0 + 1000, .nextBeatInBar = 1};
    pl_publish(&w, &o);
    s_nout = 0;
    pl_tick(&w, t0 + 1000, OUR, MASK, kMac);
    int bc = 0, uc = 0;
    for (int i = 0; i < s_nout; i++) if (s_out[i].port == PL_PORT_BEAT) { if (s_out[i].ip == PEER) uc++; else bc++; }
    CHECK(bc == 1 && uc == 1, "Wi-Fi: the beat broadcast and unicast to the peer (%d, %d)", bc, uc);
}

static void f3k_master_rules(uint32_t t0)
{
    // a_master_request_is_ignored_when_we_are_not_master
    static pl l;
    setup(&l, t0);
    const uint8_t from[4] = {0, 0, 0, 1};
    s_nout = 0;
    control(&l, t0, 0x26, 1, 0, from);
    CHECK(!find(PL_PORT_BEAT, 0x27), "not master: a request goes unanswered");
    // a_yield_is_recorded_only_while_we_asked_for_master
    control(&l, t0, 0x27, 1, 1, 0);
    CHECK(l.yieldedFrom == 0, "a yield we didn't ask for is ignored");
    // the old master keeps MASTER, with a higher sync counter, while handing to us: we hold (the handshake
    // decides, not the counter; freedj-3000 abdicating on the counter was its bug)
    xdjStatus(&l, t0 + 1000, 0xe4, 0xff);
    pl_request_master(&l, t0 + 1000);
    pl_tick(&l, t0 + 2000, OUR, MASK, kMac);
    control(&l, t0 + 3000, 0x27, 1, 1, 0);
    xdjStatus(&l, t0 + 4000, 0xe4, 3);
    CHECK(pl_is_master(&l), "handed to us");
    uint8_t s[sizeof kXdjStatus];
    memcpy(s, kXdjStatus, sizeof s);
    s[0x84] = 0; s[0x85] = 0; s[0x86] = 0; s[0x87] = 9;                          // its counter above ours
    pl_rx(&l, PL_PORT_STATUS, s, sizeof s, PEER, t0 + 5000);
    CHECK(pl_is_master(&l), "it still shows master with sync 9 vs our 2: we hold");
}

int main(void)
{
    OUR = ip4(169, 254, 3, 3); PEER = ip4(169, 254, 0, 2);
    MASK = ip4(255, 255, 0, 0);
    const uint32_t starts[2] = {1000000u, 0xFFFFFFFFu - 1500000u};   // and across the uint32 wrap
    for (int i = 0; i < 2; i++) {
        const uint32_t t0 = starts[i];
        follows_a_real_xdj(t0);
        only_beats(t0);
        announces_and_reports(t0);
        our_beats(t0);
        master_nobody_has_it(t0);
        master_handed_to_us(t0);
        master_handoff_times_out(t0);
        master_yielded_by_us(t0);
        commands(t0);
        f3k_captured_beats(t0);
        f3k_idle_status(t0);
        f3k_pre_nexus(t0);
        f3k_beats_round_trip(t0);
        f3k_twins(t0);
        f3k_master_rules(t0);
    }
    printf("prolink_test: %d checks, %d failed\n", s_n, s_fail);
    return s_fail != 0;
}
