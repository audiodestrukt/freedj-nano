// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// The CH32V003 deck on the computer: the real firmware (fw/) on emulated parts.
//
//   SD card     sim/emu/sd_emu.c on SPI, holding a FAT32 image (tools/sdimage.py)
//   OLED        sim/emu/oled_emu.c (SSD1306 128x64) parsing the I2C stream, drawn in a window
//   audio       the PWM DMA ring, played at 22,059 Hz (TIM1's rate)
//   EC11, PLAY, CUE, SHIFT, pot    the keyboard and mouse wheel, as pin levels
//   USART       a pty (tools: midi_bridge.py --port build/tty), or the DJ2Go read directly
//
//   sim                         window, keyboard, sound
//   sim --midi auto             + the DJ2Go (stop any other bridge using it first)
//   sim --script s.txt --out d  headless, scripted, faster than real time: d/sim.wav,
//                               d/<name>.png screenshots, a status line per event
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "app.h"
#include "hal.h"
#include "lite.h"
#include "oled_emu.h"
#include "sd_emu.h"
#include "ui.h"

#define RATE 22059                       // 48 MHz / 544 / 4

// ---- options
static const char* o_sd = "build/sd.img";
static const char* o_script;
static const char* o_out = "build/out";
static const char* o_midi;
static const char* o_tty = "build/tty";
static double o_spiMhz = 24, o_accessUs = 300, o_stallMs = 0;
static unsigned o_stallEvery = 0;
static int o_scale = 4;

// ---- emulated parts
static sd_emu   s_sd;
static oled_emu s_oled;
static int      s_sdFd = -1;
static double   s_spiHz = 400e3;

// ---- time: microseconds since boot
static int      s_headless;
static uint64_t s_samples;                // consumed by the "DMA"
static uint64_t s_t0;
static double nowUs(void)
{
    if (s_headless) return s_samples * 1e6 / RATE;
    return (SDL_GetPerformanceCounter() - s_t0) * 1e6 / SDL_GetPerformanceFrequency();
}

// ---- inputs
static uint8_t  s_buttons;                // PIN_PUSH | PIN_PLAY | ...
static int      s_encPending;             // quadrature steps still to make (+ clockwise)
static uint8_t  s_encPhase;               // position in the Gray cycle 00 01 11 10
static int      s_pot = 512;
static const uint8_t kGray[4] = { 0, 1, 3, 2 };

// ---- UART FIFO
static uint8_t  s_rx[1024];
static unsigned s_rxHead, s_rxTail;
static void rxPush(uint8_t b) { if (s_rxTail - s_rxHead < sizeof s_rx) s_rx[s_rxTail++ % sizeof s_rx] = b; }

// ---- audio ring and output
static uint8_t  s_ring[HAL_AUDIO_BLOCKS][2 * HAL_AUDIO_BLOCK];
static unsigned s_rd, s_filled;
static uint32_t s_underruns;
static int16_t* s_wav; static size_t s_wavN, s_wavCap;
static SDL_AudioDeviceID s_audio;

// ---- I2C
static double s_i2cDoneUs;

// ---- CPU estimate. The firmware runs natively here, so its cycles are estimated from what it
// does, at costs from sim/qemu's cycle model (make deckbench: ~2.3-2.6 cycles per instruction
// on the QingKe V2A, flash fetch and taken branches). A guide; deckbench is the measurement.
#define C_PAUSED    25      // per sample, not playing
#define C_STRAIGHT  200     // per sample, varispeed at 1.00
#define C_VARI      335     // per sample, varispeed at another speed
#define C_KEYLOCK   350     // per sample, keylock between searches
#define C_SEARCH    85000   // one keylock search and its read-ahead (every 256 samples)
#define C_SPI_BYTE  30      // a polled SPI byte: the wire + the driver
#define C_PAGE      13500   // rendering one OLED page
#define C_STEP      2000    // the rest of a main-loop step
#define RING_CYCLES (48e6 * (HAL_AUDIO_BLOCKS - 1) * HAL_AUDIO_BLOCK / RATE)
static double   s_stepC, s_totalC, s_winC, s_winPeak, s_peak, s_cpuPct, s_peakPct;
static uint16_t s_lastI;
static uint32_t s_frames, s_winFrames;    // OLED frames begun (page 0 addressed)
static double   s_fps;

// ================================================================ HAL

uint32_t hal_ms(void) { return (uint32_t)(nowUs() / 1000); }

uint8_t hal_pins(void) { return s_buttons | kGray[s_encPhase & 3]; }
uint16_t hal_pot(void) { return (uint16_t)s_pot; }

int hal_uart_rx(void) { return s_rxHead == s_rxTail ? -1 : s_rx[s_rxHead++ % sizeof s_rx]; }

static void sdTiming(void)
{
    const double bpu = s_spiHz / 8e6;     // bytes per microsecond
    s_sd.access = (uint32_t)(o_accessUs * bpu);
    s_sd.gap = (uint32_t)(o_accessUs * bpu / 4);
    s_sd.stall = (uint32_t)(o_stallMs * 1000 * bpu);
    s_sd.stallEvery = o_stallEvery;
}
void hal_sd_select(int on) { sd_emu_select(&s_sd, on); }
uint8_t hal_spi(uint8_t b) { s_stepC += C_SPI_BYTE; if (s_sdFd < 0) { s_sd.bytes++; return 0xFF; } return sd_emu_xfer(&s_sd, b); }   // no card: MISO pulled up
void hal_spi_fast(int on) { s_spiHz = on ? o_spiMhz * 1e6 : 400e3; sdTiming(); }

void hal_i2c_send(uint8_t addr, const uint8_t* p, uint16_t n)
{
    oled_emu_write(&s_oled, addr, p, n);
    if (n > 100) s_stepC += C_PAGE;
    if (n == 7 && p[0] == 0x00 && p[1] == 0x21 && p[5] == 0) { s_frames++; s_winFrames++; }
    s_i2cDoneUs = nowUs() + (n + 1) * 9 / 0.4;   // 400 kHz, 9 clocks a byte, + the address
}
int hal_i2c_busy(void) { return nowUs() < s_i2cDoneUs; }

uint8_t* hal_audio_block(void) { return s_filled < HAL_AUDIO_BLOCKS ? s_ring[(s_rd + s_filled) % HAL_AUDIO_BLOCKS] : 0; }
void hal_audio_commit(void)
{
    s_filled++;
    if (!g_deck.playing) s_stepC += HAL_AUDIO_BLOCK * C_PAUSED;
    else if (g_deck.stretch) {
        s_stepC += HAL_AUDIO_BLOCK * C_KEYLOCK;
        if (g_deck.i < s_lastI || g_deck.i == 0) s_stepC += C_SEARCH;   // a hop began: a search
    } else s_stepC += HAL_AUDIO_BLOCK * (g_deck.speed == 0x10000 ? C_STRAIGHT : C_VARI);
    s_lastI = g_deck.i;
}

// One main-loop step, with its estimated cycles.
static void step(void)
{
    s_stepC = C_STEP;
    app_step();
    s_totalC += s_stepC; s_winC += s_stepC;
    if (s_stepC > s_winPeak) s_winPeak = s_stepC;
    if (nowUs() > 100e3 && s_stepC > s_peak) s_peak = s_stepC;    // after the boot mount
}

// Per-second figures for the window.
static void cpuWindow(double secs)
{
    s_cpuPct = s_winC / (secs * 48e6) * 100;
    s_peakPct = s_winPeak / RING_CYCLES * 100;
    s_fps = s_winFrames / secs;
    s_winC = s_winPeak = 0; s_winFrames = 0;
}

// ================================================================ the "DMA": one block out

static void consumeBlock(void)
{
    int16_t s[HAL_AUDIO_BLOCK];
    if (s_filled) {
        const uint8_t* b = s_ring[s_rd];
        for (int k = 0; k < HAL_AUDIO_BLOCK; k++) s[k] = (int16_t)((b[2 * k] << 8 | b[2 * k + 1]) - 32768);
        s_rd = (s_rd + 1) % HAL_AUDIO_BLOCKS; s_filled--;
    } else {
        s_underruns++;
        memset(s, 0, sizeof s);           // TIM1 would repeat the stale duty; silence is close
    }
    s_samples += HAL_AUDIO_BLOCK;
    if (s_audio) SDL_QueueAudio(s_audio, s, sizeof s);
    if (s_headless) {
        if (s_wavN + HAL_AUDIO_BLOCK > s_wavCap) { s_wavCap = s_wavCap * 2 + 65536; s_wav = realloc(s_wav, s_wavCap * 2); }
        memcpy(s_wav + s_wavN, s, sizeof s); s_wavN += HAL_AUDIO_BLOCK;
    }
}

// One step of the encoder per app_step, so the firmware sees every edge.
static void encoderStep(void)
{
    if (!s_encPending) return;
    if (s_encPending > 0) { s_encPhase++; s_encPending--; } else { s_encPhase--; s_encPending++; }
}

// ================================================================ files

static int sdRead(void* ctx, uint32_t lba, uint8_t* dst)
{
    (void)ctx;
    return pread(s_sdFd, dst, 512, (off_t)lba * 512) == 512 ? 0 : -1;
}

static void writeWav(const char* path, const int16_t* x, size_t n)
{
    FILE* f = fopen(path, "wb"); if (!f) return;
    const uint32_t data = (uint32_t)n * 2, riff = 36 + data, fmt = 16, rate = 22050, br = rate * 2;
    const uint16_t pcm = 1, ch = 1, ba = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(x, 2, n, f); fclose(f);
}

// PNG with stored (uncompressed) deflate blocks: no zlib needed.
static uint32_t crcTab[256];
static uint32_t crc(uint32_t c, const uint8_t* p, size_t n)
{
    if (!crcTab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320 ^ (v >> 1) : v >> 1; crcTab[i] = v; }
    c = ~c; while (n--) c = crcTab[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE* f, const char* type, const uint8_t* d, uint32_t n)
{
    uint8_t h[8]; be32(h, n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f); fwrite(d, 1, n, f);
    uint32_t c = crc(0, (const uint8_t*)type, 4); c = crc(c, d, n);
    uint8_t t[4]; be32(t, c); fwrite(t, 1, 4, f);
}
static void writeShot(const char* path, int scale)
{
    const int w = 128 * scale, h = 64 * scale, stride = w * 3 + 1;
    uint8_t* raw = malloc((size_t)stride * h);
    for (int y = 0; y < h; y++) {
        uint8_t* r = raw + (size_t)y * stride; *r++ = 0;
        for (int x = 0; x < w; x++) {
            const int on = oled_emu_pixel(&s_oled, x / scale, y / scale);
            r[0] = on ? 0x9F : 0x05; r[1] = on ? 0xE8 : 0x08; r[2] = on ? 0xFF : 0x10; r += 3;
        }
    }
    const size_t n = (size_t)stride * h, blocks = (n + 65534) / 65535;
    uint8_t* z = malloc(2 + n + blocks * 5 + 4), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < n; off += 65535) {
        const uint16_t len = (uint16_t)(n - off > 65535 ? 65535 : n - off);
        *q++ = off + len >= n; *q++ = len & 255; *q++ = len >> 8; *q++ = ~len & 255; *q++ = (~len >> 8) & 255;
        memcpy(q, raw + off, len); q += len;
    }
    be32(q, b << 16 | a); q += 4;
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
        chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (uint32_t)(q - z)); chunk(f, "IEND", 0, 0);
        fclose(f);
    }
    free(raw); free(z);
}

// ================================================================ status

static void status(const char* why)
{
    const double t = nowUs() / 1e6;
    printf("t=%.3f %s count=%u track=%d state=%d pos=%ld speed=%.4f keylock=%d stretch=%d tempo=%d range=%d "
           "browse=%d sel=%d cue=%lu err=%d misses=%u underruns=%u sd_cmds=%u sd_reads=%u sd_bytes=%llu "
           "sd_stalls=%u oled_bytes=%u frames=%u cpu_est=%.1f peak_step=%.0f\n",
           t, why, g_app.count, g_app.track == APP_NONE ? -1 : g_app.track, g_app.state, (long)lite_pos(&g_deck),
           g_deck.speed / 65536.0, g_app.keylock, g_deck.stretch, g_app.tempo, g_app.range,
           g_app.browsing, g_app.sel, (unsigned long)g_app.cue, g_app.err, g_deck.misses, s_underruns,
           s_sd.cmds, s_sd.reads, (unsigned long long)s_sd.bytes, s_sd.stalls, s_oled.bytes, s_frames,
           t > 0 ? s_totalC / (t * 48e6) * 100 : 0.0, s_peak / RING_CYCLES * 100);
    fflush(stdout);
}

// ================================================================ USART sources

static int s_ptyFd = -1, s_midiFd = -1;

static void openPty(void)
{
    s_ptyFd = posix_openpt(O_RDWR | O_NOCTTY);
    if (s_ptyFd < 0 || grantpt(s_ptyFd) || unlockpt(s_ptyFd)) { perror("pty"); return; }
    const char* name = ptsname(s_ptyFd);
    int keep = open(name, O_RDWR | O_NOCTTY);              // keep the other end open: reads don't EIO
    struct termios t; if (keep >= 0 && !tcgetattr(keep, &t)) { cfmakeraw(&t); tcsetattr(keep, TCSANOW, &t); }
    fcntl(s_ptyFd, F_SETFL, O_NONBLOCK);
    unlink(o_tty);
    if (symlink(name, o_tty)) perror("symlink");
    printf("USART on %s (%s): python3 ../../freedj-esp/tools/midi_bridge.py --port %s\n", name, o_tty, o_tty);
}

static const char* findDj2go(void)
{
    static char dev[64];
    FILE* f = fopen("/proc/asound/cards", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        int card; char id[64];
        if (sscanf(line, " %d [%63[^]]", &card, id) == 2 && strstr(id, "DJ2Go")) {
            snprintf(dev, sizeof dev, "/dev/snd/midiC%dD0", card); fclose(f); return dev;
        }
    }
    if (f) fclose(f);
    return 0;
}

static uint8_t s_mStatus, s_mData[2], s_mN;
static void midiByte(uint8_t b)          // raw MIDI -> the bridge's 0x01 + USB-MIDI packet
{
    if (b >= 0xF8) return;
    if (b & 0x80) { s_mStatus = b >= 0xF0 ? 0 : b; s_mN = 0; return; }
    if (!s_mStatus) return;
    s_mData[s_mN++] = b;
    const int need = (s_mStatus & 0xE0) == 0xC0 ? 1 : 2;
    if (s_mN < need) return;
    s_mN = 0;
    rxPush(0x01); rxPush(s_mStatus >> 4); rxPush(s_mStatus); rxPush(s_mData[0]); rxPush(need > 1 ? s_mData[1] : 0);
}

static void pollSerial(void)
{
    uint8_t buf[256];
    ssize_t n;
    if (s_ptyFd >= 0) while ((n = read(s_ptyFd, buf, sizeof buf)) > 0) for (ssize_t i = 0; i < n; i++) rxPush(buf[i]);
    if (s_midiFd >= 0) while ((n = read(s_midiFd, buf, sizeof buf)) > 0) for (ssize_t i = 0; i < n; i++) midiByte(buf[i]);
}

// ================================================================ headless script

static uint8_t pinOf(const char* name)
{
    if (!strcmp(name, "play")) return PIN_PLAY;
    if (!strcmp(name, "cue")) return PIN_CUE;
    if (!strcmp(name, "push")) return PIN_PUSH;
    if (!strcmp(name, "shift")) return PIN_SHIFT;
    fprintf(stderr, "unknown button %s\n", name); exit(2);
}

typedef struct { double ms; char cmd[16], arg[200]; } Event;

static int runScript(void)
{
    FILE* f = fopen(o_script, "r");
    if (!f) { perror(o_script); return 2; }
    Event* ev = 0; int n = 0, cap = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* h = strchr(line, '#'); if (h) *h = 0;
        Event e = {0};
        if (sscanf(line, "%lf %15s %199[^\n]", &e.ms, e.cmd, e.arg) < 2) continue;
        if (n == cap) { cap = cap * 2 + 16; ev = realloc(ev, sizeof *ev * cap); }
        ev[n++] = e;
        if (!strcmp(e.cmd, "tap")) {                          // press now, release 80 ms later
            Event r = e; r.ms += 80; strcpy(r.cmd, "release"); strcpy(ev[n - 1].cmd, "press");
            if (n == cap) { cap = cap * 2 + 16; ev = realloc(ev, sizeof *ev * cap); }
            ev[n++] = r;
        }
    }
    fclose(f);
    for (int i = 1; i < n; i++)                              // stable sort by time
        for (int j = i; j > 0 && ev[j - 1].ms > ev[j].ms; j--) { Event t = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = t; }
    mkdir(o_out, 0755);
    double end = n ? ev[n - 1].ms : 0, nextStatus = 1000;
    int i = 0;
    app_init();
    for (int k = 0; k < 40; k++) { step(); consumeBlock(); }   // ~60 ms: the card mounted
    status("boot");
    while (i < n || nowUs() / 1000 <= end) {
        const double ms = nowUs() / 1000;
        while (i < n && ev[i].ms <= ms) {
            Event* e = &ev[i++];
            if (!strcmp(e->cmd, "press")) s_buttons |= pinOf(e->arg);
            else if (!strcmp(e->cmd, "release")) s_buttons &= (uint8_t)~pinOf(e->arg);
            else if (!strcmp(e->cmd, "turn")) s_encPending += 4 * atoi(e->arg);
            else if (!strcmp(e->cmd, "pot")) s_pot = atoi(e->arg);
            else if (!strcmp(e->cmd, "key")) for (const char* p = e->arg; *p; p++) rxPush((uint8_t)*p);
            else if (!strcmp(e->cmd, "midi")) { unsigned a, b, c; if (sscanf(e->arg, "%x %x %x", &a, &b, &c) == 3) { midiByte(a); midiByte(b); midiByte(c); } }
            else if (!strcmp(e->cmd, "shot")) { char p[512]; snprintf(p, sizeof p, "%s/%s.png", o_out, e->arg); writeShot(p, 4); }
            else if (strcmp(e->cmd, "mark") && strcmp(e->cmd, "end")) { fprintf(stderr, "unknown command %s\n", e->cmd); return 2; }
            char why[240]; snprintf(why, sizeof why, "[%s %s]", e->cmd, e->arg);
            status(why);
        }
        encoderStep();
        step();
        consumeBlock();                                      // 32 samples = 1.45 ms per step
        if (ms >= nextStatus) { nextStatus += 1000; status("tick"); }
    }
    char p[512]; snprintf(p, sizeof p, "%s/sim.wav", o_out);
    writeWav(p, s_wav, s_wavN);
    status("end");
    return 0;
}

// ================================================================ window

static void drawText(SDL_Renderer* r, int x, int y, int sc, const char* s)
{
    for (; *s; s++, x += 6 * sc) {
        const char c = *s < 32 || *s > 126 ? '?' : *s;
        for (int col = 0; col < 5; col++)
            for (int b = 0; b < 7; b++)
                if (ui_font[c - 32][col] >> b & 1) { SDL_Rect q = { x + col * sc, y + b * sc, sc, sc }; SDL_RenderFillRect(r, &q); }
    }
}

static void keyEvent(SDL_Keycode k, int down, int ctrl)
{
    uint8_t pin = 0;
    switch (k) {
    case SDLK_SPACE: pin = PIN_PLAY; break;
    case SDLK_c: pin = PIN_CUE; break;
    case SDLK_RETURN: pin = PIN_PUSH; break;
    case SDLK_LSHIFT: case SDLK_RSHIFT: pin = PIN_SHIFT; break;
    case SDLK_LEFT: if (down) s_encPending -= 4 * (ctrl ? 4 : 1); return;
    case SDLK_RIGHT: if (down) s_encPending += 4 * (ctrl ? 4 : 1); return;
    case SDLK_UP: if (down) s_pot = s_pot + (ctrl ? 64 : 8) > 1023 ? 1023 : s_pot + (ctrl ? 64 : 8); return;
    case SDLK_DOWN: if (down) s_pot = s_pot - (ctrl ? 64 : 8) < 0 ? 0 : s_pot - (ctrl ? 64 : 8); return;
    case SDLK_HOME: if (down) s_pot = 512; return;
    case SDLK_k: if (down) rxPush('k'); return;              // console keys, as over the bridge
    case SDLK_n: if (down) rxPush('n'); return;
    case SDLK_b: if (down) rxPush('b'); return;
    default: return;
    }
    if (down) s_buttons |= pin; else s_buttons &= (uint8_t)~pin;
}

static int runWindow(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    const int W = 128 * o_scale, H = 64 * o_scale, PAD = 12, INFO = 4 * 10 * 2;
    SDL_Window* win = SDL_CreateWindow("freedj nano: CH32V003 deck (sim)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       W + 2 * PAD, H + 3 * PAD + INFO, 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    SDL_AudioSpec want = {0}, have;
    want.freq = RATE; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 512;
    s_audio = SDL_OpenAudioDevice(0, 0, &want, &have, 0);
    if (!s_audio) fprintf(stderr, "no audio: %s\n", SDL_GetError());
    else {
        int16_t pre[1024] = {0};                             // ~45 ms of lead
        SDL_QueueAudio(s_audio, pre, sizeof pre);
        SDL_PauseAudioDevice(s_audio, 0);
    }
    s_t0 = SDL_GetPerformanceCounter();
    app_init();
    printf("keys: space PLAY, c CUE, shift SHIFT, enter push, left/right or wheel turn (ctrl: x4), "
           "up/down pot (ctrl: x8), home pot centre, k n b console keys, esc quit\n");
    uint64_t lastDraw = 0, lastBytes = 0; double lastT = 0, spiPct = 0, cmdsPerS = 0; uint32_t lastCmds = 0;
    for (int run = 1; run;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) run = 0;
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                if (e.key.keysym.sym == SDLK_ESCAPE) run = 0;
                if (!e.key.repeat || e.key.keysym.sym == SDLK_LEFT || e.key.keysym.sym == SDLK_RIGHT ||
                    e.key.keysym.sym == SDLK_UP || e.key.keysym.sym == SDLK_DOWN)
                    keyEvent(e.key.keysym.sym, e.type == SDL_KEYDOWN, !!(e.key.keysym.mod & KMOD_CTRL));
            } else if (e.type == SDL_MOUSEWHEEL) {
                s_encPending += 4 * (e.wheel.y > 0 ? 1 : -1);
            }
        }
        pollSerial();
        int did = 0;
        const uint64_t due = (uint64_t)(nowUs() * RATE / 1e6);
        if (s_samples + HAL_AUDIO_BLOCK <= due) { consumeBlock(); did = 1; }
        encoderStep();
        step();
        const uint64_t now = SDL_GetTicks();
        if (now - lastDraw >= 33) {
            lastDraw = now;
            const double t = nowUs() / 1e6;
            if (t - lastT >= 1) {
                spiPct = (s_sd.bytes - lastBytes) * 8 / s_spiHz / (t - lastT) * 100;
                cmdsPerS = (s_sd.cmds - lastCmds) / (t - lastT);
                cpuWindow(t - lastT);
                lastBytes = s_sd.bytes; lastCmds = s_sd.cmds; lastT = t;
            }
            SDL_SetRenderDrawColor(ren, 24, 24, 28, 255); SDL_RenderClear(ren);
            SDL_SetRenderDrawColor(ren, 5, 8, 16, 255);
            SDL_Rect glass = { PAD - 4, PAD - 4, W + 8, H + 8 }; SDL_RenderFillRect(ren, &glass);
            SDL_SetRenderDrawColor(ren, 0x9F, 0xE8, 0xFF, 255);
            for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++)
                if (oled_emu_pixel(&s_oled, x, y)) { SDL_Rect q = { PAD + x * o_scale, PAD + y * o_scale, o_scale - 1, o_scale - 1 }; SDL_RenderFillRect(ren, &q); }
            char l1[160], l2[160], l3[160], l4[160];
            snprintf(l1, sizeof l1, "pot %4d  %s%s%s%s", s_pot, s_buttons & PIN_PLAY ? "PLAY " : "", s_buttons & PIN_CUE ? "CUE " : "",
                     s_buttons & PIN_SHIFT ? "SHIFT " : "", s_buttons & PIN_PUSH ? "PUSH" : "");
            snprintf(l2, sizeof l2, "SD bus %4.1f%%  %4.0f cmd/s  stalls %u", spiPct, cmdsPerS, s_sd.stalls);
            snprintf(l3, sizeof l3, "CPU ~%2.0f%% est  worst step %3.0f%%", s_cpuPct, s_peakPct);
            snprintf(l4, sizeof l4, "%2.0f fps  underruns %u  misses %u", s_fps, s_underruns, g_deck.misses);
            SDL_SetRenderDrawColor(ren, 170, 170, 180, 255);
            drawText(ren, PAD, H + 2 * PAD, 2, l1);
            drawText(ren, PAD, H + 2 * PAD + 20, 2, l2);
            drawText(ren, PAD, H + 2 * PAD + 40, 2, l3);
            drawText(ren, PAD, H + 2 * PAD + 60, 2, l4);
            SDL_RenderPresent(ren);
        }
        if (!did && s_filled == HAL_AUDIO_BLOCKS) SDL_Delay(1);
    }
    SDL_Quit();
    return 0;
}

// ================================================================ main

int main(int argc, char** argv)
{
    int pty = 1;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "--sd") && v) o_sd = argv[++i];
        else if (!strcmp(a, "--script") && v) { o_script = argv[++i]; s_headless = 1; }
        else if (!strcmp(a, "--out") && v) o_out = argv[++i];
        else if (!strcmp(a, "--midi") && v) o_midi = argv[++i];
        else if (!strcmp(a, "--tty") && v) o_tty = argv[++i];
        else if (!strcmp(a, "--no-pty")) pty = 0;
        else if (!strcmp(a, "--spi-mhz") && v) o_spiMhz = atof(argv[++i]);
        else if (!strcmp(a, "--sd-access-us") && v) o_accessUs = atof(argv[++i]);
        else if (!strcmp(a, "--sd-stall-ms") && v) o_stallMs = atof(argv[++i]);
        else if (!strcmp(a, "--sd-stall-every") && v) o_stallEvery = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--scale") && v) o_scale = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--sd IMG] [--script FILE --out DIR] [--midi DEV|auto] [--tty PATH] [--no-pty]\n"
                            "  [--spi-mhz 24] [--sd-access-us 300] [--sd-stall-ms 0 --sd-stall-every N] [--scale 4]\n", argv[0]);
            return 2;
        }
    }
    s_sdFd = open(o_sd, O_RDONLY);
    if (s_sdFd >= 0) {
        struct stat st; fstat(s_sdFd, &st);
        s_sd.blocks = (uint32_t)(st.st_size / 512);
        s_sd.read = sdRead;
    } else {
        fprintf(stderr, "%s: %s (the deck will say no SD card; make sd)\n", o_sd, strerror(errno));
    }
    sd_emu_reset(&s_sd);
    if (s_sdFd < 0) s_sd.idle = 1, s_sd.initPolls = 255;      // no card: never leaves idle
    sdTiming();
    oled_emu_reset(&s_oled);
    if (o_midi) {
        const char* dev = strcmp(o_midi, "auto") ? o_midi : findDj2go();
        s_midiFd = dev ? open(dev, O_RDONLY | O_NONBLOCK) : -1;
        if (s_midiFd < 0) fprintf(stderr, "MIDI %s: %s\n", dev ? dev : "DJ2Go", dev ? strerror(errno) : "not found");
        else printf("MIDI from %s\n", dev);
    }
    if (s_headless) return runScript();
    if (pty) openPty();
    return runWindow();
}
