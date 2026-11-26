// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// tc_lookup: 20-bit timecode windows (hex, as a probe logs them) -> the cycle on the record (fw/timecode.c's
// table), to see which way a real record runs against it.   tc_lookup [vinyl|cd] 0x12345 ...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "timecode.h"
int main(int argc, char** argv)
{
    int a = 1, fmt = TC_SERATO_VINYL;
    if (a < argc && !strcmp(argv[a], "cd")) { fmt = TC_SERATO_CD; a++; }
    else if (a < argc && !strcmp(argv[a], "vinyl")) a++;
    int32_t* lut = malloc(sizeof(int32_t) << 20);
    for (uint32_t i = 0; i < 1u << 20; i++) lut[i] = -1;
    uint32_t w = tc_seed(fmt);
    for (uint32_t i = 0; i < tc_length(fmt); i++) { lut[w] = (int32_t)i; w = tc_next(fmt, w); }
    for (; a < argc; a++) {
        const uint32_t v = (uint32_t)strtoul(argv[a], 0, 0) & 0xFFFFF;
        printf("0x%05x -> %d\n", v, lut[v]);
    }
    return 0;
}
