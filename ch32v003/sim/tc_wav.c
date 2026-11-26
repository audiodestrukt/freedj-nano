// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// tc_wav: a recording of a timecode record (16-bit stereo WAV) through fw/timecode.c, every side x flags x
// level mode: how much of it is locked forwards, the longest run, where it reads. For real recordings of the
// user's records (freedj-3000's rec1.wav ...) against what the FM-1 sees.   tc_wav rec.wav
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "timecode.h"
static int32_t* s_lut[3];
static int32_t lut(uint32_t w, void* ctx) { return s_lut[*(int*)ctx][w & 0xFFFFF]; }
int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44) return 1;
    const uint32_t rate = h[24] | h[25] << 8 | h[26] << 16 | (uint32_t)h[27] << 24;
    fseek(f, 0, SEEK_END); const long n = (ftell(f) - 44) / 4; fseek(f, 44, SEEK_SET);
    int16_t* pcm = malloc(n * 4);
    if (fread(pcm, 4, n, f) != (size_t)n) return 1;
    static int sides[2] = {TC_SERATO_VINYL, TC_SERATO_VINYL_B};
    for (int s = 0; s < 2; s++) {
        const int fm = sides[s];
        s_lut[fm] = malloc(sizeof(int32_t) << 20);
        for (uint32_t i = 0; i < 1u << 20; i++) s_lut[fm][i] = -1;
        uint32_t w = tc_seed(fm);
        for (uint32_t i = 0; i < tc_length(fm); i++) { s_lut[fm][w] = (int32_t)i; w = tc_next(fm, w); }
    }
    printf("%s: %u Hz, %.1f s\n", argv[1], rate, (double)n / rate);
    for (int s = 0; s < 2; s++) for (int lv = 0; lv < 3; lv++) for (uint32_t fl = 0; fl < 8; fl++) {
        static tc_dec d;
        tc_begin(&d, rate, sides[s], lut, &sides[s]);
        tc_flags(&d, fl); tc_level(&d, lv);
        const uint32_t hop = rate / 100;
        uint32_t fwd = 0, rev = 0, best = 0, hops = 0;
        int32_t p0 = -1, p1 = -1;
        for (long i = 0; i + (long)hop <= n; i += hop, hops++) {
            tc_push(&d, pcm + 2 * i, hop);
            if (d.valid > best) best = d.valid;
            if (d.locked) { if (d.speed > 0) fwd++; else rev++; if (d.position >= 0) { if (p0 < 0) p0 = d.position; p1 = d.position; } }
        }
        if (fwd + rev < hops / 20 && fl != 0) continue;   // (quiet combinations: only flags 0 shown)
        printf("  side %c level %-6s flags %u: locked fwd %3u%% rev %3u%%, best run %6u, positions %d .. %d\n",
               s ? 'B' : 'A', lv == 0 ? "sample" : lv == 1 ? "mean" : "peak", fl, 100 * fwd / hops, 100 * rev / hops, best, p0, p1);
    }
    return 0;
}
