// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// The deck firmware for RV32EC under qemu-riscv32 (user mode), for an instruction count of
// the whole main loop: the same HAL as sim/host, on the same emulated SD card and SSD1306
// (compiled in; count.py leaves their instructions out), with a scripted session. The card
// image comes in through Linux syscalls. Time is the audio consumed, one block per app_step.
#include "app.h"
#include "hal.h"
#include "oled_emu.h"
#include "sd_emu.h"

#define RATE   22059
#define SPI_HZ 24000000

// ---- syscalls (RVE: the number in t0)
static long sys(long n, long a, long b, long c, long d, long e)
{
    register long t0 __asm__("t0") = n;
    register long a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c, a3 __asm__("a3") = d, a4 __asm__("a4") = e;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(t0), "r"(a1), "r"(a2), "r"(a3), "r"(a4) : "memory");
    return a0;
}
static void put(const char* s) { long n = 0; while (s[n]) n++; sys(64, 2, (long)s, n, 0, 0); }   // stderr: stdout is the trace
static void putn(uint32_t v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); put(b + i); }

// ---- the session: what a person does, in ms
enum { E_PRESS, E_RELEASE, E_TURN, E_POT, E_END };
typedef struct { uint32_t ms; uint8_t type, arg; int16_t n; } Ev;
static const Ev kScript[] = {
    {  500, E_PRESS, PIN_PLAY, 0 }, { 580, E_RELEASE, PIN_PLAY, 0 },          // play, varispeed
    { 2500, E_POT, 0, 920 },                                                     // +7.9 %
    { 3500, E_PRESS, PIN_PUSH, 0 }, { 3580, E_RELEASE, PIN_PUSH, 0 },          // keylock on
    { 6500, E_PRESS, PIN_SHIFT, 0 }, { 6550, E_TURN, 0, 2 },                   // the list, next row
    { 6700, E_RELEASE, PIN_SHIFT, 0 },
    { 6800, E_PRESS, PIN_PUSH, 0 }, { 6880, E_RELEASE, PIN_PUSH, 0 },          // load track 2
    { 7200, E_PRESS, PIN_PLAY, 0 }, { 7280, E_RELEASE, PIN_PLAY, 0 },
    { 8500, E_PRESS, PIN_CUE, 0 }, { 8580, E_RELEASE, PIN_CUE, 0 },            // back to the cue
    { 8800, E_TURN, 0, 10 },                                                     // search
    { 9300, E_PRESS, PIN_CUE, 0 }, { 9800, E_RELEASE, PIN_CUE, 0 },            // preview
    { 10500, E_END, 0, 0 },
};

static sd_emu   s_sd;
static oled_emu s_oled;
static long     s_fd;
static uint32_t s_samples, s_ms, s_msAcc, s_i2cDone;
static uint8_t  s_buttons, s_phase;
static int16_t  s_enc;
static uint16_t s_pot = 512;
static uint8_t  s_ring[HAL_AUDIO_BLOCKS][2 * HAL_AUDIO_BLOCK];
static uint8_t  s_rd, s_filled;
static uintptr_t s_spTop, s_spMin;
static const uint8_t kGray[4] = { 0, 1, 3, 2 };

static void sp(void)
{
    uintptr_t v;
    __asm__ volatile("mv %0, sp" : "=r"(v));
    if (v < s_spMin) s_spMin = v;
}

// ---- HAL
uint32_t hal_ms(void) { return s_ms; }
uint8_t hal_pins(void) { return s_buttons | kGray[s_phase & 3]; }
uint16_t hal_pot(void) { return s_pot; }
int hal_uart_rx(void) { return -1; }
void hal_sd_select(int on) { sd_emu_select(&s_sd, on); }
void spi_bus_byte(void);
uint8_t hal_spi(uint8_t b) { sp(); spi_bus_byte(); return sd_emu_xfer(&s_sd, b); }
void hal_spi_fast(int on)
{
    const uint32_t bpu = on ? SPI_HZ / 8000000 : 1;          // bytes per microsecond (400 kHz: ~0)
    s_sd.access = on ? 300 * bpu : 15;                        // 300 us
    s_sd.gap = s_sd.access / 4;
}
void hal_i2c_send(uint8_t addr, const uint8_t* p, uint16_t n)
{
    sp();
    oled_emu_write(&s_oled, addr, p, n);
    s_i2cDone = s_samples + ((uint32_t)(n + 1) >> 1) + 1;     // 400 kHz: ~0.5 sample per byte
}
int hal_i2c_busy(void) { return s_samples < s_i2cDone; }
uint8_t* hal_audio_block(void) { sp(); return s_filled < HAL_AUDIO_BLOCKS ? s_ring[(s_rd + s_filled) % HAL_AUDIO_BLOCKS] : 0; }
void hal_audio_commit(void) { s_filled++; }

static int readBlock(void* ctx, uint32_t lba, uint8_t* dst)
{
    (void)ctx;
    long long r;
    if (sys(62, s_fd, (long)(lba >> 23), (long)(lba << 9), (long)&r, 0)) return -1;   // llseek
    return sys(63, s_fd, (long)dst, 512, 0, 0) == 512 ? 0 : -1;
}

// count.py splits the trace here: one call per audio block consumed.
__attribute__((noinline)) void sim_block_mark(void) { __asm__ volatile(""); }
__attribute__((noinline)) void sim_event_mark(void) { __asm__ volatile(""); }

int qemu_main(void)
{
    s_fd = sys(56, -100, (long)"build/simtest/sd.img", 0, 0, 0);   // openat(AT_FDCWD, ..., O_RDONLY)
    if (s_fd < 0) { put("no build/simtest/sd.img (make simtest)\n"); return 2; }
    long long size = 0;
    sys(62, s_fd, 0, 0, (long)&size, 2);                    // llseek to the end
    s_sd.read = readBlock;
    s_sd.blocks = (uint32_t)(size >> 9);
    sd_emu_reset(&s_sd);
    oled_emu_reset(&s_oled);
    hal_spi_fast(0);
    __asm__ volatile("mv %0, sp" : "=r"(s_spTop));
    s_spMin = s_spTop;

    app_init();
    sim_block_mark();
    unsigned i = 0;
    for (;;) {
        while (kScript[i].ms <= s_ms) {
            const Ev* e = &kScript[i++];
            if (e->type == E_END) goto done;
            if (e->type == E_PRESS) s_buttons |= e->arg;
            else if (e->type == E_RELEASE) s_buttons &= (uint8_t)~e->arg;
            else if (e->type == E_TURN) s_enc += 4 * e->n;
            else if (e->type == E_POT) s_pot = (uint16_t)e->n;
            sim_event_mark();
        }
        if (s_enc) { if (s_enc > 0) { s_phase++; s_enc--; } else { s_phase--; s_enc++; } }
        app_step();
        // the DMA: one block out
        if (s_filled) { s_rd = (s_rd + 1) % HAL_AUDIO_BLOCKS; s_filled--; }
        s_samples += HAL_AUDIO_BLOCK;
        s_msAcc += HAL_AUDIO_BLOCK * 1000;
        while (s_msAcc >= RATE) { s_msAcc -= RATE; s_ms++; }
        sim_block_mark();
    }
done:
    put("session "); putn(s_ms); put(" ms: track "); putn(g_app.track + 1u); put(", state "); putn(g_app.state);
    put(", keylock "); putn(g_app.keylock); put(", misses "); putn(g_deck.misses);
    put(", SD commands "); putn(s_sd.cmds); put(", SD bytes "); putn((uint32_t)s_sd.bytes);
    put(", OLED bytes "); putn(s_oled.bytes);
    put("\nstack: "); putn((uint32_t)(s_spTop - s_spMin)); put(" bytes below qemu_main\n");
    return 0;
}

__attribute__((naked, section(".text.start"))) void _start(void)
{
    __asm__ volatile(".option push\n .option norelax\n la gp, __global_pointer$\n .option pop\n"
                     "call qemu_main\n li t0, 93\n ecall\n");
}
