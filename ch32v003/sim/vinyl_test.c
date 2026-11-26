// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// vinyl_test: the timecode vinyl rules (fw/vinyl.c) and the decoder (fw/timecode.c) they rely on, with a
// synthetic Serato Control Vinyl: a quadrature 1 kHz carrier whose loud/quiet cycles spell the record's LFSR
// sequence, so position, direction and speed are known. freedj-esp's native/vinyl_rules_test.cpp in C: each
// case is a fix found on a turntable with freedj-3000's DVS (2026-11-24).
//
//   make vinyltest      (exit 0 = all pass)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "timecode.h"
#include "vinyl.h"

enum { SR = 48000 };
static const double kStartCycles = 24000;         // track 0:00 = 24.0 s of timecode on Dan's record

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// the table (window -> cycle) and the bit each cycle carries (the MSB of the LFSR state there)
// per side (TC_SERATO_VINYL, TC_SERATO_VINYL_B): s_rec the record on the platter, s_dec what the deck reads it as
static int32_t* s_luts[3];
static uint8_t* s_bitss[3];
static int s_rec = TC_SERATO_VINYL, s_dec = TC_SERATO_VINYL;
static int32_t lut(uint32_t w, void* ctx) { (void)ctx; return s_luts[s_dec][w & 0xFFFFF]; }
static void build(int f)
{
    const uint32_t n = tc_length(f);
    int32_t* l = s_luts[f] = malloc(sizeof(int32_t) << 20);
    uint8_t* b = s_bitss[f] = malloc(n);
    for (uint32_t i = 0; i < 1u << 20; i++) l[i] = -1;
    uint32_t w = tc_seed(f);
    for (uint32_t i = 0; i < n; i++) { l[w] = (int32_t)i; b[i] = (w >> 19) & 1; w = tc_next(f, w); }
}

// a record under a needle: phase in cycles (1 cycle = 1 bit = 1 ms at 33); primary (R) cos, secondary (L) sin
typedef struct { double phase; } record;
static void render(record* r, double speed, int frames, int16_t* out)
{
    const double step = speed * 1000.0 / SR;
    const long n = (long)tc_length(s_rec);
    const uint8_t* bits = s_bitss[s_rec];
    for (int i = 0; i < frames; i++) {
        long c = (long)floor(r->phase);
        if (c < 0) c = 0;
        if (c >= n) c = n - 1;
        const double amp = bits[c] ? 20000.0 : 14000.0, a = r->phase * 2.0 * M_PI;
        out[2 * i] = (int16_t)(17000.0 * sin(a));
        out[2 * i + 1] = (int16_t)(amp * cos(a));
        r->phase += step;
    }
}

// the deck's vinyl input: 10 ms windows through the decoder and the rules; time is the sample count
typedef struct {
    tc_dec dec;
    vinyl_win win;
    uint32_t nowUs;
    int absolute;
    vinyl_rec last;
} turntable;
static void tt_init(turntable* t)
{
    memset(t, 0, sizeof *t);
    tc_begin(&t->dec, SR, s_dec, lut, 0);
    vinyl_win_init(&t->win);
    t->absolute = 1;
}
static void tt_window(turntable* t, const int16_t* buf, int frames)
{
    int16_t peak = 0;
    for (int i = 0; i < 2 * frames; i++) { int a = abs(buf[i]); if (a > 32767) a = 32767; if (a > peak) peak = (int16_t)a; }
    tc_push(&t->dec, buf, (uint32_t)frames);
    t->nowUs += (uint32_t)((int64_t)frames * 1000000 / SR);
    t->last = *vinyl_step(&t->win, t->nowUs, peak, &t->dec, t->absolute);
}
static void play(turntable* t, record* r, double speed, double secs)
{
    static int16_t buf[2 * SR / 100];
    for (int i = 0; i < (int)lround(secs * 100); i++) { render(r, speed, SR / 100, buf); tt_window(t, buf, SR / 100); }
}
static void silence(turntable* t, double secs)
{
    static const int16_t buf[2 * SR / 100];
    for (int i = 0; i < (int)lround(secs * 100); i++) tt_window(t, buf, SR / 100);
}
static int placed(const turntable* t) { return t->dec.locked && t->dec.position >= 0; }
static double pos_ms(const turntable* t) { return placed(t) ? t->dec.position - kStartCycles : -1; }

// ── The decoder ──

static void backwards_reads_the_needle(void)
{
    printf("backwards reads the needle, not 19 ms ahead\n");
    turntable t; tt_init(&t);
    record r = {100000};
    play(&t, &r, 1.0, 0.5);
    const double sps[] = {-1.0, -0.5, -2.0};
    for (int i = 0; i < 3; i++) {
        play(&t, &r, sps[i], 0.5);
        CHECK(t.dec.locked, "locked at %+.1fx", sps[i]);
        CHECK(abs(t.dec.position - (int)r.phase) <= 1, "at %+.1fx: read %d, the needle is at %d", sps[i], t.dec.position, (int)r.phase);
    }
}

static void relock_is_quick(void)
{
    printf("tc_relock places a needle drop in ~45 cycles; tc_reset needs the level again\n");
    turntable t; tt_init(&t);
    record a = {50000};
    play(&t, &a, 1.0, 0.5);
    tc_relock(&t.dec);
    record b = {300000};
    int ms = 0;
    for (; ms < 500 && !placed(&t); ms += 10) play(&t, &b, 1.0, 0.01);
    CHECK(ms <= 60, "after relock: a position in %d ms (20 bits to fill, 24 to trust)", ms);
    CHECK(abs(t.dec.position - (int)b.phase) <= 2, "at the new place: %d vs %d", t.dec.position, (int)b.phase);
    tc_reset(&t.dec);
    record c = {500000};
    int slow = 0;
    for (; slow < 1000 && !placed(&t); slow += 10) play(&t, &c, 1.0, 0.01);
    CHECK(slow > ms, "a full reset is slower (%d ms vs %d ms): relock is what a needle drop wants", slow, ms);
}

// ── The record: arming, lifts, settling ──

static void arms_on_a_steady_record_only(void)
{
    printf("arms after 0.3 s of a steady 33, never on a wobble\n");
    turntable t; tt_init(&t);
    record r = {kStartCycles + 60000};
    play(&t, &r, 1.0, 0.2);
    CHECK(!t.last.armed, "not after 0.2 s");
    play(&t, &r, 1.0, 0.2);
    CHECK(t.last.armed, "armed by 0.4 s");
    turntable w; tt_init(&w);
    record m = {0};
    for (int i = 0; i < 100; i++) play(&w, &m, 1.5 + 1.2 * sin(i * 0.4), 0.01);
    CHECK(!w.last.armed, "a wobbling tone (music) never arms");
}

static void a_hand_holding_the_platter_is_not_a_lift(void)
{
    printf("back-cueing: 300 ms holds between strokes never go silent\n");
    turntable t; tt_init(&t);
    record r = {kStartCycles + 60000};
    play(&t, &r, 1.0, 0.6);
    const uint32_t drops = t.last.drops;
    const double sps[] = {-1.5, 1.5, -1.0, 2.0};
    for (int i = 0; i < 4; i++) {
        silence(&t, 0.3);
        play(&t, &r, sps[i], 0.03);
        CHECK(!t.last.settling, "%+.1f: not settling", sps[i]);
        CHECK(fabs(t.last.speed - sps[i]) < 0.3, "%+.1f: plays at once (read %+.2f)", sps[i], t.last.speed);
        play(&t, &r, sps[i], 0.2);
    }
    CHECK(t.last.drops == drops, "no needle drops counted (%u -> %u)", drops, t.last.drops);
}

static void a_needle_drop_settles_briefly(void)
{
    printf("a needle drop: silent until placed, within 60 ms\n");
    turntable t; tt_init(&t);
    record a = {kStartCycles + 10000};
    play(&t, &a, 1.0, 0.6);
    const uint32_t drops = t.last.drops;
    silence(&t, 0.6);                             // lifted
    record b = {kStartCycles + 200000};
    play(&t, &b, 1.0, 0.02);
    CHECK(t.last.settling && t.last.speed == 0.0f, "settling, silent");
    play(&t, &b, 1.0, 0.04);
    CHECK(!t.last.settling, "placed by 60 ms");
    CHECK(t.last.drops == drops + 1, "counted as a drop");
    CHECK(fabs(pos_ms(&t) - (b.phase - kStartCycles)) < 3, "at the new place: %.0f vs %.0f", pos_ms(&t), b.phase - kStartCycles);
}

// ── ABS: keeping the deck on the needle ──

// the deck's audio loop in miniature: 5.8 ms blocks, the speed smoothed 0.35 a block, a playhead moved by it
#define BLOCK_MS (256 * 1000.0f / 44100.0f)
typedef struct {
    vinyl_follow f;
    float posMs, smooth, biggestStepMs;
    int jumps, dropJumps;
} deck;
static void deck_init(deck* d) { memset(d, 0, sizeof *d); vinyl_follow_init(&d->f); }
static void block(deck* d, float raw, int has, float needleMs, uint32_t drops)
{
    d->smooth += 0.35f * (raw - d->smooth);
    const vinyl_out o = vinyl_follow_step(&d->f, raw, d->smooth, BLOCK_MS, has, needleMs, d->posMs, drops);
    const float before = d->posMs;
    if (o.seek) { d->posMs = o.seekMs; d->jumps++; if (o.drop) d->dropJumps++; }
    else d->posMs += (d->smooth + o.bend) * BLOCK_MS;
    if (!o.seek && fabsf(d->posMs - before) > d->biggestStepMs) d->biggestStepMs = fabsf(d->posMs - before);
}

static void follow_places_on_first_lock_then_keeps_up(void)
{
    printf("ABS: the first lock places the deck; steady play never jumps\n");
    deck d; deck_init(&d);
    float needle = 10000;
    block(&d, 1.0f, 1, needle, 0);
    CHECK(d.jumps == 1 && fabsf(d.posMs - needle) < 1e-3f, "placed at the needle");
    for (int i = 0; i < 1000; i++) { needle += BLOCK_MS; block(&d, 1.0f, 1, needle, 0); }
    CHECK(d.jumps == 1, "no more jumps in 5.8 s of play (%d)", d.jumps);
    CHECK(fabsf(d.posMs - needle) < 5, "on the needle: %.1f ms off", d.posMs - needle);
}

static void follow_never_jumps_while_back_cueing(void)
{
    printf("ABS: rocking the record never jumps\n");
    deck d; deck_init(&d);
    float needle = 30000;
    block(&d, 1.0f, 1, needle, 0);
    for (int i = 0; i < 400; i++) {               // 2.3 s of rocking, +-1.8x
        const float sp = (float)(1.8 * sin(i * BLOCK_MS / 1000.0 * 6.0));
        needle += sp * BLOCK_MS;
        block(&d, sp, 1, needle + (i % 7) * 15.0f, 0);   // a noisy needle position on top
    }
    CHECK(d.jumps == 1, "only the first placement (%d jumps)", d.jumps);
    CHECK(d.biggestStepMs < 12, "the playhead moved only as the hand did (biggest step %.1f ms)", d.biggestStepMs);
}

static void follow_jumps_on_drops_bends_small_gaps(void)
{
    printf("ABS: a drop jumps; a 40 ms gap bends shut; a big gap under a steady record jumps once\n");
    deck d; deck_init(&d);
    float needle = 10000;
    block(&d, 1.0f, 1, needle, 0);
    needle = 60000;
    block(&d, 1.0f, 1, needle, 1);
    CHECK(d.dropJumps == 2 && fabsf(d.posMs - needle) < 1e-3f, "the drop jumped to the needle");
    d.posMs -= 40;                                // 40 ms behind after a scratch
    int bent = 0;
    for (int i = 0; i < 800; i++) {
        needle += BLOCK_MS;
        const float before = d.posMs;
        block(&d, 1.0f, 1, needle, 1);
        if (d.posMs - before > BLOCK_MS * 1.001f) bent = 1;
    }
    CHECK(bent, "closed by bending, not a jump");
    CHECK(d.jumps == 2, "no jump for 40 ms (%d)", d.jumps);
    CHECK(fabsf(d.posMs - needle) < VINYL_DEAD_MS + 1, "closed: %.1f ms off", d.posMs - needle);
    d.posMs += 30000;                             // moved 30 s under a steady record
    for (int i = 0; i < 5; i++) { needle += BLOCK_MS; block(&d, 1.0f, 1, needle, 1); }
    CHECK(d.jumps == 3 && fabsf(d.posMs - needle) < 10, "one jump back to the needle (%d, %.0f off)", d.jumps, d.posMs - needle);
}

static void side_b(void)
{
    printf("side B: read as side B it plays forwards and places; read as side A it never places\n");
    s_rec = s_dec = TC_SERATO_VINYL_B;
    turntable t; tt_init(&t);
    record r = {400000};
    play(&t, &r, 1.0, 0.5);
    CHECK(placed(&t) && t.dec.speed > 0.9f, "side B as B: speed %+.2f, position %d", t.dec.speed, t.dec.position);
    CHECK(abs(t.dec.position - (int)r.phase) <= 1, "side B at %d, read %d", (int)r.phase, t.dec.position);
    // the deck's choice of side and flags: the one that locks with the speed positive on a record playing a
    // steady 1x (nobody holds a record backwards at 33). Side B is side A's code backwards, so side B read as
    // side A locks - consistently, a real LFSR run - but only reading backwards (the FM-1 saw this, 2026-11-26).
    s_dec = TC_SERATO_VINYL;
    int fwdLock = 0, revLock = 0;
    for (uint32_t f = 0; f < 8; f++) {
        turntable a; tt_init(&a); tc_flags(&a.dec, f);
        record q = {400000};
        play(&a, &q, 1.0, 0.5);
        printf("    flags %u: locked %d speed %+.2f valid %u\n", f, a.dec.locked, a.dec.speed, a.dec.valid);
        if (a.dec.locked && a.dec.speed > 0.6f) fwdLock = 1;
        if (a.dec.locked && a.dec.speed < -0.6f) revLock = 1;
    }
    CHECK(!fwdLock, "side B read as side A never locks forwards (any flags)");
    CHECK(revLock, "side B read as side A locks backwards with some flags (as on the FM-1)");
    s_rec = s_dec = TC_SERATO_VINYL;
}

// the record on the platter through a chain that may invert the secondary (L) channel: what the scan picks
static void scan_case(int side, int invertL, uint32_t wantFlags)
{
    static vinyl_scan sc;
    vinyl_scan_begin(&sc, SR);
    s_rec = side;
    record r = {300000};
    static int16_t buf[2 * SR / 100];
    uint32_t us = 0;
    int ms = 0;
    for (; ms < 2000; ms += 10) {
        render(&r, 1.0, SR / 100, buf);
        if (invertL) for (int i = 0; i < SR / 100; i++) buf[2 * i] = (int16_t)-buf[2 * i];
        us += 10000;
        if (vinyl_scan_push(&sc, buf, SR / 100, us)) break;
    }
    const int gotSide = vinyl_scan_side(&sc);
    const uint32_t gotFlags = vinyl_scan_flags(&sc);
    printf("  side %c%s: found in %d ms: side %c flags %u\n", side == TC_SERATO_VINYL ? 'A' : 'B', invertL ? ", L inverted" : "",
           ms, gotSide == TC_SERATO_VINYL ? 'A' : 'B', gotFlags);
    CHECK(sc.found >= 0 && ms <= 800, "found within 0.8 s (%d ms)", ms);
    CHECK(gotSide == side, "the right side");
    CHECK(gotFlags == wantFlags, "flags %u (want %u)", gotFlags, wantFlags);
    // and that pick reads the record where it is
    s_dec = gotSide;
    turntable t; tt_init(&t); tc_flags(&t.dec, gotFlags);
    record q = {300000};
    for (int k = 0; k < 50; k++) {
        static int16_t b2[2 * SR / 100];
        render(&q, 1.0, SR / 100, b2);
        if (invertL) for (int i = 0; i < SR / 100; i++) b2[2 * i] = (int16_t)-b2[2 * i];
        tt_window(&t, b2, SR / 100);
    }
    CHECK(placed(&t) && abs(t.dec.position - (int)q.phase) <= 2, "reads %d, the needle at %d", t.dec.position, (int)q.phase);
    s_rec = s_dec = TC_SERATO_VINYL;
}

static void scan_finds_the_side(void)
{
    printf("the scan: which side, and the flags the chain needs\n");
    scan_case(TC_SERATO_VINYL, 0, 0);
    scan_case(TC_SERATO_VINYL_B, 0, 0);
    scan_case(TC_SERATO_VINYL_B, 1, TC_SWITCH_PHASE);
    scan_case(TC_SERATO_VINYL, 1, TC_SWITCH_PHASE);
}

static void clock_wraps(void)
{
    printf("the microsecond clock wrapping (71 min) neither lifts nor drops the needle\n");
    turntable t; tt_init(&t);
    t.nowUs = 0xFFFFFFFFu - 200000;               // 0.2 s before the wrap
    record r = {kStartCycles + 60000};
    play(&t, &r, 1.0, 0.6);
    const uint32_t drops = t.last.drops;
    silence(&t, 0.3);
    play(&t, &r, 1.0, 0.3);
    CHECK(t.last.signal && !t.last.settling && t.last.drops == drops, "a hold across the wrap is a hold");
    CHECK(t.last.armed, "armed across the wrap");
}

int main(void)
{
    build(TC_SERATO_VINYL);
    build(TC_SERATO_VINYL_B);
    backwards_reads_the_needle();
    relock_is_quick();
    arms_on_a_steady_record_only();
    a_hand_holding_the_platter_is_not_a_lift();
    a_needle_drop_settles_briefly();
    follow_places_on_first_lock_then_keeps_up();
    follow_never_jumps_while_back_cueing();
    follow_jumps_on_drops_bends_small_gaps();
    clock_wraps();
    side_b();
    scan_finds_the_side();
    printf("-- the decoder and the rules again, on side B --\n");
    s_rec = s_dec = TC_SERATO_VINYL_B;
    backwards_reads_the_needle();
    relock_is_quick();
    a_hand_holding_the_platter_is_not_a_lift();
    a_needle_drop_settles_briefly();
    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
