// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// TCBENCH: an fm1-dock probe: fw/timecode.c (integer, Q16) against the same per-sample work in float (as
// freedj-esp's src/timecode.cpp has it, with the FPU on: -mcpu=r3 -mfprev1), on a synthetic 1 kHz stereo
// carrier at 48 kHz. Prints ns per stereo frame for each, and what 48 kHz of it costs (% of the core).
//
//   cd ~/sandbox/thirdparty/fm1-dock && uv run --with python-rtmidi tools/probe.py run \
//       ~/sandbox/audiodestrukt/freedj-nano/ch32v003/fm1/probes/tcbench.c --on fm1
/* hbmod-cflags: -O2 -mcpu=r3 -mfprev1 -ffp-contract=off */
#include "probe.h"
#include "../../fw/timecode.c"

enum { N = 4800 };                                // 100 ms
static int16_t s_buf[2 * N];

// ---- the float variant: freedj-esp's chan / sample / bit, as floats where it had them
typedef struct { int positive, swapped; float zero; uint32_t ticker; } fchan;
typedef struct {
    fchan pri, sec; uint32_t since; int forwards; float speed, ref, refLevel, alpha;
    uint32_t bitstream, expected, valid, rate;
} fdec;
static void fch(fchan* c, float v, float a)
{
    c->ticker++; c->swapped = 0;
    if (v > c->zero + 128.f && !c->positive) { c->swapped = 1; c->positive = 1; c->ticker = 0; }
    else if (v < c->zero - 128.f && c->positive) { c->swapped = 1; c->positive = 0; c->ticker = 0; }
    c->zero += a * (v - c->zero);
}
static void fbit(fdec* d, float m)
{
    if (d->refLevel == 0.f) d->refLevel = 1e9f;
    const uint32_t b = m > d->refLevel;
    d->expected = fwd(d->expected, 0x34d54, 20);
    d->bitstream = (d->bitstream >> 1) | (b << 19);
    if (d->expected == d->bitstream) d->valid++; else { d->expected = d->bitstream; d->valid = 0; }
    d->refLevel += (m - d->refLevel) * (1.f / 48.f);
}
static void fpush(fdec* d, const int16_t* lr, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const float p = (float)lr[2 * i + 1], s = (float)lr[2 * i];
        fch(&d->pri, p, d->alpha); fch(&d->sec, s, d->alpha);
        if (d->pri.swapped || d->sec.swapped) {
            const int f = d->pri.swapped ? d->pri.positive != d->sec.positive : d->pri.positive == d->sec.positive;
            if (f != d->forwards) { d->forwards = f; d->valid = 0; }
        }
        d->since++;
        if (d->pri.swapped) {
            const uint32_t h = d->since; d->since = 0;
            if (h) { float r = d->ref / (2.f * (float)h); if (!d->forwards) r = -r; d->speed = 0.85f * d->speed + 0.15f * r; }
        }
        if (d->sec.swapped && d->pri.positive) { float m = p - d->pri.zero; fbit(d, m < 0 ? -m : m); }
    }
}

static int32_t run(hb_probe_ctx_t* c)
{
    // a 1 kHz carrier, L leading R by a quarter turn, its level stepping as bits would (loud / quiet)
    static const int16_t kSin[48] = {0, 1044, 2079, 3090, 4067, 5000, 5878, 6691, 7431, 8090, 8660, 9135, 9511,
        9781, 9945, 10000, 9945, 9781, 9511, 9135, 8660, 8090, 7431, 6691, 5878, 5000, 4067, 3090, 2079, 1044, 0,
        -1044, -2079, -3090, -4067, -5000, -5878, -6691, -7431, -8090, -8660, -9135, -9511, -9781, -9945, -10000,
        -9945, -9781};
    for (uint32_t i = 0; i < N; i++) {
        const int32_t k = (int32_t)(i % 48u), loud = (i / 48u) & 1 ? 1 : 2;
        s_buf[2 * i] = (int16_t)(kSin[(k + 12) % 48] * loud / 2);
        s_buf[2 * i + 1] = (int16_t)(kSin[k] * loud / 2);
    }
    static tc_dec d;
    tc_begin(&d, 48000, TC_SERATO_VINYL, 0, 0);
    static fdec f;
    f.alpha = 1.f / 49.f; f.ref = 48.f; f.forwards = 1;
    p_irq_off();
    uint32_t t0 = p_ticks();
    for (int k = 0; k < 10; k++) tc_push(&d, s_buf, N);
    const uint32_t ti = p_ticks() - t0;
    t0 = p_ticks();
    for (int k = 0; k < 10; k++) fpush(&f, s_buf, N);
    const uint32_t tf = p_ticks() - t0;
    p_irq_on();
    // ticks are 24 MHz: ns per frame = ticks * 1000 / 24 / (10 N)
    p_u32(c, "int_ns_per_frame_x10", ti * 100u / 24u / N);     // (x10: one decimal)
    p_u32(c, "float_ns_per_frame_x10", tf * 100u / 24u / N);
    p_u32(c, "int_pct_at_48k_x100", ti / 24u / 10u);           // 1 s of 48 kHz = 10 runs of N: us in 1 s, /100 -> %
    p_u32(c, "float_pct_at_48k_x100", tf / 24u / 10u);
    p_i32(c, "int_speed_x1000", (int32_t)(d.speed * 1000.f));
    p_i32(c, "float_speed_x1000", (int32_t)(f.speed * 1000.f));
    p_u32(c, "int_valid", d.valid);
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "TCBENCH", run};
