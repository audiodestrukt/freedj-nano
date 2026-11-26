// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// linksync_test: fw/linksync.c against freedj-3000's peer_phase_tests and sync_offset_tests
// (crates/app/src/prodj.rs), the same sequences and limits; plus a long run (float precision over hours) and the
// clock wrapping.   make prolinktest
#include <math.h>
#include <stdio.h>
#include "linksync.h"

static int s_fail, s_n;
#define CHECK(c, ...) do { s_n++; if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define BPM 120.0f                                // 500 ms a beat
#define DT  (1.0 / 60.0)                          // one 60 Hz frame
static uint32_t s_base;                           // the clock's zero (us), to run across the wrap too

// frames from fromMs to toMs, the packets ARRIVING at arrivals[] (ms); phase per frame into ph[] (if given)
static int run(ls_peer* p, double fromMs, double toMs, const double* arr, int narr, int playing, float* ph, double* at)
{
    int n = 0;
    for (double t = fromMs; t < toMs; t += DT * 1000.0) {
        const double now = floor(t);
        double beat = -1;
        for (int i = 0; i < narr; i++) if (arr[i] <= now && arr[i] > beat) beat = arr[i];
        uint8_t shown;
        const float f = ls_peer_advance(p, s_base + (uint32_t)(now * 1000.0), s_base + (uint32_t)(beat * 1000.0), beat >= 0, BPM, 0,
                                        playing, (float)DT, &shown);
        if (ph) ph[n] = f;
        if (at) at[n] = now;
        n++;
    }
    return n;
}
static float step(float a, float b) { const float d = b - a; return d - floorf(d); }

static void steady_beats_run_at_tempo(void)
{
    ls_peer p; ls_peer_init(&p);
    double arr[20]; for (int i = 0; i < 20; i++) arr[i] = 1000 + i * 500;
    static float ph[1000];
    const int n = run(&p, 1000, 8000, arr, 20, 1, ph, 0);
    int bad = 0;
    for (int i = 6; i < n; i++) bad += fabsf(step(ph[i - 1], ph[i]) - 1.0f / 30.0f) >= 0.01f;
    CHECK(!bad, "1/30 beat a frame at 120 BPM, 60 Hz (%d frames off)", bad);
}

static void late_bunched_packets_do_not_stall_the_row(void)
{
    ls_peer p; ls_peer_init(&p);
    double arr[20];
    for (int i = 0; i < 20; i++) { const double due = 1000 + i * 500; arr[i] = due + (400 - fmin(fmod(due, 1400), 400)); }
    static float ph[1000];
    const int n = run(&p, 1000, 11000, arr, 20, 1, ph, 0);
    const float inc = (float)(DT / 0.5);
    int bad = 0;
    for (int i = 11; i < n; i++) {
        const float s = step(ph[i - 1], ph[i]);
        bad += s < inc * (1 - LS_SLEW) - 1e-4f || s > inc * (1 + LS_SLEW) + 1e-4f;
    }
    CHECK(!bad, "late, bunched beats (0-400 ms): never stalled, backwards or jumping (%d frames off)", bad);
}

static void a_packet_forty_ms_late_moves_phase_by_a_quarter_of_the_error(void)
{
    ls_peer p; ls_peer_init(&p);
    double arr[11]; for (int i = 0; i < 10; i++) arr[i] = 1000 + i * 500;
    arr[10] = 6040;                                // the 6 s beat, 40 ms late
    const int n = run(&p, 1000, 8000, arr, 11, 1, 0, 0);
    const double freeRun = (n - 1.0) * DT / 0.5;
    const double moved = ls_peer_phase(&p) - freeRun, want = -0.08 * LS_PULL;
    CHECK(fabs(moved - want) < 0.003, "40 ms late (0.08 beat): moved %.4f, a quarter is %.4f", moved, want);
    CHECK(fabsf(p.pending) < 1e-6f, "the correction all bled in (%g)", p.pending);
}

static void a_paused_peer_holds_after_two_periods(void)
{
    ls_peer p; ls_peer_init(&p);
    double arr[6]; for (int i = 0; i < 6; i++) arr[i] = 1000 + i * 500;     // the last beat at 3500
    run(&p, 1000, 3600, arr, 6, 1, 0, 0);
    static float ph[400]; static double at[400];
    const int n = run(&p, 3600, 6000, arr, 6, 0, ph, at);                   // status: paused, no more beats
    int moving = 0;
    for (int i = 1; i < n; i++) if (at[i - 1] > 4600 && ph[i] != ph[i - 1]) moving++;
    CHECK(!moving, "paused, no beat for two periods: it holds (%d frames moved)", moving);
}

static void a_playing_peer_with_late_beats_keeps_running(void)
{
    ls_peer p; ls_peer_init(&p);
    double arr[6]; for (int i = 0; i < 6; i++) arr[i] = 1000 + i * 500;
    run(&p, 1000, 3600, arr, 6, 1, 0, 0);
    static float ph[400];
    const int n = run(&p, 3600, 6600, arr, 6, 1, ph, 0);                    // PLAY, but beats stop for 3 s
    int stalled = 0;
    for (int i = 1; i < n; i++) stalled += !(step(ph[i - 1], ph[i]) > 0);
    CHECK(!stalled, "status says PLAY: it keeps running without beats");
}

static void bar_count_turns_over_with_the_phase_not_the_packet(void)
{
    // beats every 500 ms from 1 s, beat in bar 1,2,3,4,1..., each 0-24 ms late or early (a fixed pattern)
    double arr[40]; uint8_t bib[40];
    for (int i = 0; i < 40; i++) { arr[i] = 1000 + i * 500 + (double)((i * 7919) % 25) - 12; bib[i] = (uint8_t)(i % 4 + 1); }
    for (int pass = 0; pass < 2; pass++) {
        ls_peer p; ls_peer_init(&p);
        double prev = -1;
        int jumps = 0;
        uint8_t shown = 0;
        const double end = pass ? 10250 : 20000;
        for (double t = 1000; t < end; t += DT * 1000.0) {
            const double now = floor(t);
            int k = -1;
            for (int i = 0; i < 40; i++) if (arr[i] <= now) k = i;
            const float f = ls_peer_advance(&p, s_base + (uint32_t)(now * 1000), k >= 0 ? s_base + (uint32_t)(arr[k] * 1000) : 0, k >= 0, BPM,
                                            k >= 0 ? bib[k] : 0, 1, (float)DT, &shown);
            const double bar = (shown - 1.0) + f;
            if (prev >= 0) { double s = fmod(bar - prev, 4.0); if (s < 0) s += 4; if (s >= 0.1) jumps++; }
            prev = bar;
        }
        if (!pass) CHECK(!jumps, "the bar position never jumps on a packet a few ms off (%d)", jumps);
        else CHECK(shown == 3, "and it's the master's bar: beat 3 at 10 s (%u)", shown);
    }
}

static void long_run_keeps_its_precision(void)
{
    // (ours: a free run of 3 hours at 174 BPM, 31320 beats: the whole beats apart from the fraction, so a frame's
    // step is as exact at the end as at the start)
    ls_peer p; ls_peer_init(&p);
    const float bpm = 174.0f;
    const double per = 60000.0 / bpm;
    double last = 1000;
    float prevF = 0, worst = 0;
    for (double t = 1000; t < 1000 + 3 * 3600e3; t += DT * 1000.0) {
        if (t - last >= per) last += per;
        uint8_t shown;
        const float f = ls_peer_advance(&p, s_base + (uint32_t)(t * 1000), s_base + (uint32_t)(last * 1000), 1, bpm, 0, 1, (float)DT, &shown);
        if (t > 3 * 3600e3 - 1000) { const float e = fabsf(step(prevF, f) - (float)(DT * bpm / 60.0)); if (e > worst) worst = e; }
        prevF = f;
    }
    CHECK(worst < 1e-4f, "3 hours in, a frame's step still exact (worst %.2g beat)", worst);
    CHECK(p.k > 31000, "the whole beats counted (%d)", p.k);
}

// ── SyncPhase ──
static void without_a_nudge_sync_pulls_to_the_beat(void)
{
    ls_sync s = {0};
    float o;
    CHECK(!ls_sync_tick(&s, 1, 0), "synced");
    CHECK(ls_sync_on_master_beat(&s, 0.1f, 0, 0, &o) == LS_CORRECT && o < 0, "ahead: slow down");
    CHECK(ls_sync_on_master_beat(&s, 0.9f, 0, 0, &o) == LS_CORRECT && o > 0, "behind: speed up");
    CHECK(ls_sync_on_master_beat(&s, 0.005f, 0, 0, &o) == LS_NONE, "inside the dead band");
}

static void a_nudge_while_synced_is_held_not_undone(void)
{
    ls_sync s = {0};
    float o;
    const float fr[3] = {0.05f, 0.12f, 0.2f};
    int none = 1;
    for (int i = 0; i < 3; i++) { ls_sync_tick(&s, 1, 1); none &= ls_sync_on_master_beat(&s, fr[i], 0, 1, &o) == LS_NONE; }
    CHECK(none, "while jogging: hands off");
    ls_sync_tick(&s, 1, 0);
    CHECK(ls_sync_on_master_beat(&s, 0.2f, 0, 0, &o) == LS_HOLD && fabsf(o - 0.2f) < 1e-6f, "settled: hold 0.2");
    CHECK(ls_sync_on_master_beat(&s, 0.2f, 0.2f, 0, &o) == LS_NONE, "at the held offset: on target");
    CHECK(ls_sync_on_master_beat(&s, 0.3f, 0.2f, 0, &o) == LS_CORRECT && o < 0, "past it: toward the offset, not 0");
    CHECK(ls_sync_on_master_beat(&s, 0.1f, 0.2f, 0, &o) == LS_CORRECT && o > 0, "0.1 is behind a 0.2 hold");
}

static void a_backward_nudge_holds_a_negative_offset(void)
{
    ls_sync s = {0};
    float o;
    ls_sync_tick(&s, 1, 1);
    ls_sync_tick(&s, 1, 0);
    CHECK(ls_sync_on_master_beat(&s, 0.85f, 0, 0, &o) == LS_HOLD && fabsf(o + 0.15f) < 1e-6f, "0.85 is 0.15 early (%g)", o);
}

static void sync_off_clears_the_hold_and_forgets_a_pending_nudge(void)
{
    ls_sync s = {0};
    float o;
    ls_sync_tick(&s, 1, 1);
    CHECK(ls_sync_tick(&s, 0, 0), "SYNC off: clear the offset");
    ls_sync_tick(&s, 1, 0);
    CHECK(ls_sync_on_master_beat(&s, 0.2f, 0, 0, &o) == LS_CORRECT, "no stale hold from before");
}

static void closed_loop_settles_on_the_held_offset(void)
{
    const float c[3][3] = {{0.5f, 0.2f, 0.0f}, {0.0f, 0.2f, 0.004f}, {0.9f, -0.15f, -0.003f}};
    for (int k = 0; k < 3; k++) {
        ls_sync s = {0};
        float frac = c[k][0];
        for (int i = 0; i < 80; i++) {
            ls_sync_tick(&s, 1, 0);
            float o = 0;
            const float r = frac - floorf(frac);
            if (ls_sync_on_master_beat(&s, r, c[k][1], 0, &o) != LS_CORRECT) o = 0;
            frac += o + c[k][2];
        }
        const float err = ls_phase_err(frac - floorf(frac), c[k][1]);
        CHECK(fabsf(err) < 0.02f, "start %.2f hold %.2f drift %.3f: ends %+.3f off", c[k][0], c[k][1], c[k][2], err);
    }
}

static void phase_err_wraps_around_the_held_offset(void)
{
    CHECK(fabsf(ls_phase_err(0.1f, 0) - 0.1f) < 1e-6f && fabsf(ls_phase_err(0.9f, 0) + 0.1f) < 1e-6f, "+ just past, - just before");
    CHECK(fabsf(ls_phase_err(0.2f, 0.2f)) < 1e-6f && fabsf(ls_phase_err(0.1f, 0.2f) + 0.1f) < 1e-6f, "around a +0.2 hold");
    CHECK(fabsf(ls_phase_err(0.8f, -0.2f)) < 1e-6f && fabsf(ls_phase_err(0.05f, -0.2f) - 0.25f) < 1e-6f, "-0.2 wraps across the beat");
    int out = 0;
    for (int i = 0; i < 100; i++) for (int j = -5; j <= 5; j++) out += fabsf(ls_phase_err((float)i / 100.0f, (float)j / 10.0f)) > 0.5f + 1e-6f;
    CHECK(!out, "always within half a beat");
}

static void correction_is_half_the_error_at_most_three_percent(void)
{
    // (ours: the gain and the cap themselves; the closed loop above settles with any gain up to 1)
    ls_sync s = {0};
    float o = 0;
    ls_sync_on_master_beat(&s, 0.04f, 0, 0, &o);
    CHECK(fabsf(o + 0.02f) < 1e-6f, "0.04 beat ahead: -2 %% for a beat (%g)", o);
    ls_sync_on_master_beat(&s, 0.3f, 0, 0, &o);
    CHECK(fabsf(o + 0.03f) < 1e-6f, "0.3 ahead: capped at -3 %% (%g)", o);
    ls_sync_on_master_beat(&s, 0.7f, 0, 0, &o);
    CHECK(fabsf(o - 0.03f) < 1e-6f, "0.3 behind: +3 %% (%g)", o);
}

static void the_bar_follows_a_master_that_moves_its_bar(void)
{
    // (ours: a master cued or looped onto another beat of the bar: its packets' beat in the bar shifts by one;
    // the bar shown follows from the next packet)
    ls_peer p; ls_peer_init(&p);
    uint8_t shown = 0;
    for (double t = 1000; t < 9000; t += DT * 1000.0) {
        const double now = floor(t);
        const int k = (int)((now - 1000) / 500);
        const double at = 1000 + k * 500.0;
        const uint8_t bib = (uint8_t)((k + (at >= 5000 ? 1 : 0)) % 4 + 1);    // from 5 s, one beat on
        ls_peer_advance(&p, s_base + (uint32_t)(now * 1000), s_base + (uint32_t)(at * 1000), 1, BPM, bib, 1, (float)DT, &shown);
        if (now >= 4800 && now < 4900) CHECK(shown == (uint8_t)(7 % 4 + 1), "before: beat %u", shown);
    }
    const int k = (int)((9000 - DT * 1000 - 1000) / 500);
    CHECK(shown == (uint8_t)((k + 1) % 4 + 1), "after the shift: the master's new bar (%u, want %d)", shown, (k + 1) % 4 + 1);
}

static void tempo_match(void)
{
    CHECK(fabsf(ls_tempo_match(126.0f, 120.0f, 0.16f) - 1.05f) < 1e-6f, "126 on a 120 track: +5 %%");
    CHECK(fabsf(ls_tempo_match(180.0f, 120.0f, 0.16f) - 1.16f) < 1e-6f, "out of range: clamped to +16 %%");
    CHECK(ls_tempo_match(0, 120.0f, 0.16f) == 1.0f, "no master tempo: unity");
}

int main(void)
{
    const uint32_t bases[2] = {0, 0xFFFFFFFFu - 3000000u};   // and across the uint32 wrap
    for (int i = 0; i < 2; i++) {
        s_base = bases[i];
        steady_beats_run_at_tempo();
        late_bunched_packets_do_not_stall_the_row();
        a_packet_forty_ms_late_moves_phase_by_a_quarter_of_the_error();
        a_paused_peer_holds_after_two_periods();
        a_playing_peer_with_late_beats_keeps_running();
        bar_count_turns_over_with_the_phase_not_the_packet();
    }
    s_base = 0;
    long_run_keeps_its_precision();
    without_a_nudge_sync_pulls_to_the_beat();
    a_nudge_while_synced_is_held_not_undone();
    a_backward_nudge_holds_a_negative_offset();
    sync_off_clears_the_hold_and_forgets_a_pending_nudge();
    closed_loop_settles_on_the_held_offset();
    phase_err_wraps_around_the_held_offset();
    tempo_match();
    correction_is_half_the_error_at_most_three_percent();
    the_bar_follows_a_master_that_moves_its_bar();
    printf("linksync_test: %d checks, %d failed\n", s_n, s_fail);
    return s_fail != 0;
}
