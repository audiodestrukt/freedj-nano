// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// tc_test: fw/timecode.c on a recording (freedj-esp's timecode.wav: Serato Control CD, 44.1 kHz), and on
// synthetic carriers. Prints, every second of a stretch, the speed, the lock and the position (cycles, and as
// seconds of timecode), then checks: a lock within 1 s, the speed 1.00 +- 1 %, the position moving 1000 a
// second, and the same with the table answering late (an FM-1 reading it off the stick): --late N bits.
//
//   cc -O2 -I../fw tc_test.c ../fw/timecode.c -o /tmp/tc_test && /tmp/tc_test ~/.../timecode.wav [secs] [skip]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "timecode.h"

static int32_t* s_lut;                            // window -> cycle (2^20)
static int s_late, s_wait;                        // answer -2 this many times first (a late table)
static uint32_t s_asks;
static int32_t lut(uint32_t w, void* ctx)
{
    (void)ctx;
    s_asks++;
    if (s_wait > 0) { s_wait--; return -2; }
    s_wait = s_late;
    return s_lut[w & 0xFFFFF];
}
static void build(int fmt)
{
    if (!s_lut) s_lut = malloc(sizeof(int32_t) << 20);
    for (uint32_t i = 0; i < 1u << 20; i++) s_lut[i] = -1;
    uint32_t w = tc_seed(fmt);
    for (uint32_t i = 0; i < tc_length(fmt); i++) { s_lut[w] = (int32_t)i; w = tc_next(fmt, w); }
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void carrier(double hz, double phase, double expect)
{
    tc_dec d;
    tc_begin(&d, 44100, TC_SERATO_CD, 0, 0);
    enum { N = 22050 };
    static int16_t b[2 * N];
    for (int i = 0; i < N; i++) {
        const double t = 2 * M_PI * hz * i / 44100.0;
        b[2 * i] = (int16_t)(8000 * sin(t)); b[2 * i + 1] = (int16_t)(8000 * sin(t + phase));
    }
    tc_push(&d, b, N);
    const double s = d.speed;
    printf("  carrier %6.0f Hz phase %+4.0f: speed %+6.3f (want %+.3f)\n", hz, phase * 180 / M_PI, s, expect);
    CHECK(fabs(s - expect) < 0.02, "speed");
}

static void wavrun(const char* path, int secs, int skip, int late)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("  (no %s: skipped)\n", path); return; }
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44) { fclose(f); return; }
    const uint32_t rate = h[24] | h[25] << 8 | h[26] << 16 | (uint32_t)h[27] << 24;
    fseek(f, 44 + (long)skip * rate * 4, SEEK_SET);
    s_late = late; s_wait = late; s_asks = 0;
    tc_dec d;
    tc_begin(&d, rate, TC_SERATO_CD, lut, 0);
    tc_flags(&d, TC_SWITCH_PRIMARY);              // (this recording: as freedj-esp's tc_flags_scan found)
    static int16_t b[2 * 4410];
    int32_t firstLock = -1, p0 = -1, p1 = -1;
    double smin = 9, smax = -9;
    printf("  %s from %d s, %d s, table %s:\n", path, skip, secs, late ? "late" : "at once");
    for (int k = 0; k < secs * 10; k++) {         // 100 ms at a time
        if (fread(b, 4, 4410 * rate / 44100, f) == 0) break;
        tc_push(&d, b, 4410 * rate / 44100);
        if (d.locked && firstLock < 0) firstLock = k * 100;
        if (k % 10 == 9) {
            const double s = d.speed;
            printf("    %3d s: speed %+6.3f %s position %7d (%.3f s)\n", k / 10 + 1, s, d.locked ? "locked" : "      ",
                   d.position, d.position / 1000.0);
            if (k >= 19) { if (s < smin) smin = s; if (s > smax) smax = s; }
            if (k == 19) p0 = d.position;
            p1 = d.position;
        }
    }
    fclose(f);
    printf("    lock at %d ms, speed %.3f..%.3f, table asked %u times\n", firstLock, smin, smax, s_asks);
    CHECK(firstLock >= 0 && firstLock <= 1000, "no lock within 1 s");
    CHECK(smin > 0.99 && smax < 1.01, "speed off 1.00");
    const int32_t want = (secs - 2) * 1000;
    CHECK(p0 >= 0 && abs((p1 - p0) - want) <= 20, "position moved %d, want %d", p1 - p0, want);
    CHECK(p0 >= 0 && abs(p0 - (skip + 2) * 1000) <= 1000, "position %d at %d s", p0, skip + 2);
}

int main(int argc, char** argv)
{
    printf("timecode (fw/timecode.c)\n");
    carrier(1000, M_PI / 2, 1.0); carrier(1000, -M_PI / 2, -1.0);      // (L leading R: forwards)
    carrier(500, M_PI / 2, 0.5); carrier(1500, M_PI / 2, 1.5); carrier(2000, -M_PI / 2, -2.0);
    const char* wav = argc > 1 ? argv[1] : "timecode.wav";
    const int secs = argc > 2 ? atoi(argv[2]) : 8, skip = argc > 3 ? atoi(argv[3]) : 60;
    build(TC_SERATO_CD);
    wavrun(wav, secs, skip, 0);
    wavrun(wav, secs, skip, 40);                  // ~40 bits late: a stick read
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
