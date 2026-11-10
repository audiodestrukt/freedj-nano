// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// Runs nano-lite under qemu-riscv32 (rv32e) for an instruction count. bench_mark()
// is called after every 64-sample block, so the trace splits per block.
#include "lite.h"

#ifndef MODE
#define MODE 3
#endif
#define FRAMES (22050 * 2)
#define N      4096

static uint8_t track[FRAMES];
static lite_deck deck;
static int16_t out[64];
volatile int32_t sink;

static void readTrack(void* ctx, uint32_t off, uint8_t* dst, uint8_t n)
{
    (void)ctx;
    for (uint8_t k = 0; k < n; k++) dst[k] = track[off + k];
}

__attribute__((noinline)) void bench_mark(void) { __asm__ volatile(""); }

static int32_t tri(int32_t i, int32_t period, int32_t amp)   // triangle wave, no multiply in the loop
{
    int32_t ph = i;
    while (ph >= period) ph -= period;
    int32_t v = ph < period / 2 ? ph : period - ph;           // 0 .. period/2
    return ((v * amp) / (period / 2)) - amp / 2;
}

int bench_main(void)
{
    for (int32_t i = 0; i < FRAMES; i++)                     // 50 Hz "bass" + 441 Hz
        track[i] = lite_ulaw_encode((int16_t)(tri(i, 441, 16000) + tri(i, 50, 8000)));
    lite_src src = { readTrack, 0, FRAMES };
    lite_init(&deck, &src);
#if MODE == 1
    lite_set_speed(&deck, 0x10000);
#elif MODE == 2
    lite_set_speed(&deck, 0x10000 + 0x10000 * 8 / 100);
#elif MODE == 3
    lite_set_keylock(&deck, 1);
    lite_set_speed(&deck, 0x10000 + 0x10000 * 8 / 100);
#endif
    lite_play(&deck, 1);
    bench_mark();
#if MODE != 0
    for (int b = 0; b < N / 64; b++) {
        lite_render(&deck, out, 64);
        sink += out[7];
        bench_mark();
    }
#endif
    return deck.misses;
}

__attribute__((naked, section(".text.start"))) void _start(void)
{
    __asm__ volatile("call bench_main\n li t0, 93\n ecall\n");   // exit(misses); RVE: syscall number in t0
}
