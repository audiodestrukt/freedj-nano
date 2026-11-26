// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// TCSELF: an fm1-dock probe: fw/timecode.c and the side scan (fw/vinyl.c) on the FM-1 itself, against a
// synthetic Serato record made here (no USB, no turntable): a 1 kHz quadrature carrier at 48 kHz whose loud /
// quiet cycles spell side A's (then side B's) LFSR, as sim/vinyl_test.c makes it. On the computer this locks
// at once and stays locked; if it doesn't here, the decoder misbehaves on the chip (the FPU build).
//
//   cd ~/sandbox/thirdparty/fm1-dock && uv run --with python-rtmidi tools/probe.py run \
//       ~/sandbox/audiodestrukt/freedj-nano/ch32v003/fm1/probes/tcself.c --on fm1
/* hbmod-cflags: -O2 -mcpu=r3 -mfprev1 -ffp-contract=off */
#include "probe.h"
#include "../../fw/timecode.c"
#include "../../fw/vinyl.c"

static const int16_t kSin[48] = {0, 1044, 2079, 3090, 4067, 5000, 5878, 6691, 7431, 8090, 8660, 9135, 9511,
    9781, 9945, 10000, 9945, 9781, 9511, 9135, 8660, 8090, 7431, 6691, 5878, 5000, 4067, 3090, 2079, 1044, 0,
    -1044, -2079, -3090, -4067, -5000, -5878, -6691, -7431, -8090, -8660, -9135, -9511, -9781, -9945, -10000,
    -9945, -9781};

// 256 frames of the record from sample index *t on: L = 1.7 sin, R = (2.0 | 1.4) cos, the bit the state's MSB
static void chunk(int16_t* b, uint32_t* t, uint32_t* state, int fmt)
{
    for (uint32_t i = 0; i < 256u; i++, (*t)++) {
        const uint32_t k = *t % 48u;
        if (k == 0 && *t) *state = tc_next(fmt, *state);
        const int32_t amp = (*state >> 19) & 1 ? 20 : 14;
        b[2 * i] = (int16_t)(kSin[k] * 17 / 10);
        b[2 * i + 1] = (int16_t)(kSin[(k + 12) % 48] * amp / 10);
    }
}

static int32_t run(hb_probe_ctx_t* c)
{
    static int16_t b[2 * 256];
    for (int side = 0; side < 2; side++) {
        const int fmt = side ? TC_SERATO_VINYL_B : TC_SERATO_VINYL;
        static tc_dec d;
        tc_begin(&d, 48000, fmt, 0, 0);
        static vinyl_scan sc;
        vinyl_scan_begin(&sc, 48000);
        uint32_t t = 0, state = tc_seed(fmt), lockedChunks = 0, chunks = 0, best = 0, foundAt = 0;
        for (int i = 0; i < 2 * 48000 / 256; i++) {           // 2 s
            c->wdt_feed();
            chunk(b, &t, &state, fmt);
            tc_push(&d, b, 256);
            if (!foundAt && vinyl_scan_push(&sc, b, 256, t / 48u * 1000u)) foundAt = t / 48u;
            chunks++;
            if (d.locked) lockedChunks++;
            if (d.valid > best) best = d.valid;
        }
        if (side == 0) {
            p_u32(c, "A_locked_pct", lockedChunks * 100u / chunks); p_u32(c, "A_best_run", best);
            p_i32(c, "A_speed_x1000", (int32_t)(d.speed * 1000.f)); p_hex(c, "A_window", d.bitstream);
            p_i32(c, "A_scan_found", sc.found); p_u32(c, "A_scan_ms", foundAt);
        } else {
            p_u32(c, "B_locked_pct", lockedChunks * 100u / chunks); p_u32(c, "B_best_run", best);
            p_i32(c, "B_speed_x1000", (int32_t)(d.speed * 1000.f)); p_hex(c, "B_window", d.bitstream);
            p_i32(c, "B_scan_found", sc.found); p_u32(c, "B_scan_ms", foundAt);
        }
    }
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "TCSELF", run};
