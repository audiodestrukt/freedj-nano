// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// USBIRQ: an fm1-dock probe: the USB controller's own interrupt (IRQ 9, which fm1-dock leaves unused) as a
// 1 kHz clock, and USB audio in from it. As usbaudio.c (a countdown, host mode, the hubs, the UCA202 found
// and set streaming), then: fm1-dock's own USB held off (as FREEDJ STICK does), our handler on vector 9 at
// priority 5 (over the audio interrupt, 3, and TIMER5, 4), the SIE's start-of-frame interrupt on, and the
// interrupts back on. Each start of frame the handler takes the last frame's packet (the DMA long done) into
// a ring and asks for the next: no waiting. The main loop, free meanwhile, runs fw/timecode.c on the ring
// for 5 s. Kept: interrupts, packets, misses, errors, the longest gap between two interrupts and the longest
// handler, and the decoder's speed / lock / position each half second.
//
//   cd ~/sandbox/thirdparty/fm1-dock && uv run --with python-rtmidi tools/probe.py run \
//       ~/sandbox/audiodestrukt/freedj-nano/ch32v003/fm1/probes/usbirq.c --on fm1 --args 20 --ms 90000
/* hbmod-cflags: -O2 -mcpu=r3 -mfprev1 -ffp-contract=off */
#include "probe.h"
#define UMSC_NO_AUDIO
#include "../usbmsc.c"
#include "../../fw/timecode.c"
#include "../../fw/vinyl.c"

#define EP3_RADR (*(volatile uint32_t *)0x11830u)
#define VEC      ((volatile uint32_t *)0x01C7FE00u)    // fm1-dock's vectors (hal/fm1_irq.h)
#define ICFG(n)  (*(volatile uint32_t *)(0x1EEF100u + 4u * ((n) >> 3)))
#define IRQ_USB  8u                                    // IRQ_USB_SOF_IDX (the SDK's wl82 hwi.h; 9 is the SIE's, CON0 SIEIE)
#define SOFIE (1u << 10)                               // CON0: the SOF interrupt on; CLR_SOF_PND bit 12, SOF_PND bit 13
#define CLR_SOF_PND (1u << 12)
#define DOCK_USB 0x01C10F68u                           // fm1-dock 0.9.0's usb struct (hal_fm1.c's FM1_STICK)
// fm1-dock write-protects the top of RAM (0x01C7FD50..: boot info, mailbox, the vectors; hal/fm1_guard.h): a write
// there is a CPU exception (DBG_MSG bit 13). Its own BLE opens it to attach vectors, as here.
#define DBG_WR_EN    (*(volatile uint32_t *)0x1EEE240u)
#define DBG_MSG_CLR  (*(volatile uint32_t *)0x1EEE248u)
#define C0_WRLIM_EN  (*(volatile uint32_t *)0x1EEE348u)
static void vec_set(uint32_t n, uint32_t v)            // (interrupts off)
{
    if (!(DBG_WR_EN & 1u)) DBG_WR_EN = 0xE7u;          // fm1__dbg_unlock
    const uint32_t en = C0_WRLIM_EN;
    C0_WRLIM_EN = en & ~4u;                            // the top window off
    VEC[n] = v;
    __asm__ volatile("csync" ::: "memory");
    DBG_MSG_CLR = 0xFFFFFFFFu;
    C0_WRLIM_EN = en;                                  // and on again
    if (DBG_WR_EN & 1u) DBG_WR_EN = 0xE7u;             // fm1__dbg_lock
}

static hb_probe_ctx_t* C;
static uint32_t pticks(void) { return p_ticks(); }
static void pidle(void) { C->wdt_feed(); }
static const umsc_env kEnv = {pticks, pidle};

static const char kHex[] = "0123456789ABCDEF";
static char* hex(char* p, uint32_t v, int digits) { while (digits--) *p++ = kHex[(v >> (4 * digits)) & 15]; return p; }
static char* dec(char* p, uint32_t v) { char t[10]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) *p++ = t[--n]; return p; }
static char* str(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static void line(int row, const char* s, uint16_t c)
{
    C->host->fill(0, 4 + row * 18, 240, 18, 0);
    C->host->text(4, 4 + row * 18, 232, 0, s, c, 0);
}
#define WHITE  HB_RGB(235, 235, 235)
#define YELLOW HB_RGB(255, 210, 0)
#define GREEN  HB_RGB(80, 220, 110)
#define RED    HB_RGB(255, 70, 50)

static int ctrl_out(const uint8_t setup[8], const uint8_t* data, uint32_t n)
{
    sie_wr(S_INDEX, 0);
    for (int i = 0; i < 8; i++) s_ep0[i] = setup[i];
    __asm__ volatile("csync" ::: "memory");
    EP0_ADR = (uint32_t)(uintptr_t)s_ep0; EP0_CNT = 8; wait_us(50);
    sie_wr(S_CSR0, 0x0A);
    int r = ep0_wait(0x02, 0x00);
    if (r) return r * 10 - 1;
    for (uint32_t i = 0; i < n; i++) s_ep0[i] = data[i];
    __asm__ volatile("csync" ::: "memory");
    EP0_ADR = (uint32_t)(uintptr_t)s_ep0; EP0_CNT = n; wait_us(50);
    sie_wr(S_CSR0, 0x02);
    r = ep0_wait(0x02, 0x00);
    if (r) return r * 10 - 2;
    sie_wr(S_CSR0, 0x60);
    r = ep0_wait(0x01, 0x01);
    sie_wr(S_CSR0, 0x00);
    return r ? r * 10 - 3 : 0;
}

// ---- the handler, and what it shares with the main loop
enum { RING = 4096 };                                  // stereo frames (85 ms at 48 kHz), a power of two
static int16_t s_ring[2 * RING];
static volatile uint32_t s_wr, s_rd;
static volatile uint32_t s_irqs, s_sofs, s_pk, s_miss, s_err, s_over, s_gapMax, s_gapOver, s_durMax, s_bytes;
static volatile uint32_t s_last, s_lastCsr, s_intr, s_pend;
static uint8_t s_arx[1024 + 8] __attribute__((aligned(8)));
static uint32_t s_ch = 2, s_sb = 2;

static volatile uint32_t s_rxIrqs, s_sinceRx, s_rearm;
static uint8_t s_arx2[1024 + 8] __attribute__((aligned(8)));
static uint8_t* const s_buf[2] = {s_arx, s_arx2};
static volatile uint32_t s_mode;                       // 0 ping-pong (take the previous packet), 1 wait 60 us and take this one
static uint32_t s_cur, s_prevN;                        // the buffer being filled; the last packet's length (0: none)
static volatile uint32_t s_nMax;
static void take(const uint8_t* q0, uint32_t n)        // a packet, its DMA long done: into the ring
{
    const uint32_t fb = s_sb * s_ch;
    for (uint32_t i = 0; i + fb <= n; i += fb) {
        if (s_wr - s_rd >= RING) { s_over++; break; }
        const uint8_t* q = q0 + i;
        const uint32_t k = s_wr & (RING - 1u);
        s_ring[2 * k] = (int16_t)(q[0] | q[1] << 8);
        s_ring[2 * k + 1] = (int16_t)(q[s_sb] | q[s_sb + 1] << 8);
        s_wr++;
    }
}
// EP3's packet is in. Its DMA may still be writing (the blocking probe needed 50 us): so the next request goes
// to the other buffer at once, and the packet taken is the previous frame's, written a millisecond ago.
static void rx_packet(void)
{
    sie_wr(S_INDEX, 3);
    const uint32_t v = sie_rd(S_RXCSR1);
    uint32_t n = 0;
    if (v & 0x01u) {
        n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) & 7u) << 8;
        if (n > 1024u) n = 1024u;
        if (n > s_nMax) s_nMax = n;
        if (v & 0x0Cu) { s_err++; s_lastCsr = v; }
        s_pk++; s_bytes += n;
        sie_wr(S_RXCSR1, 0x10);                        // FlushFIFO
    } else if (v & 0x4Cu) { s_err++; s_lastCsr = v; sie_wr(S_RXCSR1, 0); }
    if (s_mode) {                                      // mode 1: let the DMA finish, take this packet, the same buffer again
        const uint32_t t = p_ticks();
        while (p_ticks() - t < 24u * 60u) {}
        sie_wr(S_RXCSR1, 0x20);
        sie_wr(S_INDEX, 1);
        __asm__ volatile("ssync" ::: "memory");
        if (n) take(s_buf[s_cur], n);
        s_prevN = 0;
        s_sinceRx = 0;
        return;
    }
    const uint32_t done = s_cur;
    s_cur ^= 1u;
    EP3_RADR = (uint32_t)(uintptr_t)s_buf[s_cur];
    sie_wr(S_RXCSR1, 0x20);                            // ReqPkt: into the other buffer, at the next frame's start
    sie_wr(S_INDEX, 1);
    __asm__ volatile("ssync" ::: "memory");
    if (s_prevN) take(s_buf[done ^ 1u], s_prevN);      // (the one before: done ^ 1 is the buffer filled before it)
    s_prevN = n;
    s_sinceRx = 0;
}
// IRQ 9, the SIE's: EP3 received (INTRRX1 bit 3, read: cleared)
void sie_irq_body(void)
{
    const uint32_t t0 = p_ticks();
    const uint32_t rx = sie_rd(4);
    (void)sie_rd(S_INTRUSB); (void)sie_rd(2);
    if (rx & 0x08u) { s_rxIrqs++; rx_packet(); }
    const uint32_t d = p_ticks() - t0;
    if (d > s_durMax) s_durMax = d;
}
void usb_irq_body(void)
{
    const uint32_t t0 = p_ticks();
    s_irqs++;
    CON0 |= CLR_SOF_PND;                               // (the SOF interrupt's pending flag)
    s_intr |= CON0 >> 8 & 0xFFu;
    if (s_last) {
        const uint32_t g = t0 - s_last;
        if (g > s_gapMax) s_gapMax = g;
        if (g > 24000u * 12u / 10u) s_gapOver++;       // over 1.2 ms
    }
    s_last = t0;
    s_sofs++;
    if (++s_sinceRx > 3u) { s_rearm++; s_miss++; rx_packet(); }   // (no packet for 3 frames: ask again)
    const uint32_t d = p_ticks() - t0;
    if (d > s_durMax) s_durMax = d;
}
// the entry: as fm1-dock's FM1_ISR (hal/fm1_isr.S): what a C call may clobber saved, csync before rti
__asm__(".section .text.isr_usb,\"ax\",@progbits\n"
        "\t.globl isr_usb\n"
        "isr_usb:\n"
        "\t[--sp] = {psr, rets, reti}\n"
        "\t[--sp] = {r3-r0}\n"
        "\tcall usb_irq_body\n"
        "\t{r3-r0} = [sp++]\n"
        "\t{psr, rets, reti} = [sp++]\n"
        "\tcsync\n"
        "\trti\n"
        ".section .text.isr_sie,\"ax\",@progbits\n"
        "\t.globl isr_sie\n"
        "isr_sie:\n"
        "\t[--sp] = {psr, rets, reti}\n"
        "\t[--sp] = {r3-r0}\n"
        "\tcall sie_irq_body\n"
        "\t{r3-r0} = [sp++]\n"
        "\t{psr, rets, reti} = [sp++]\n"
        "\tcsync\n"
        "\trti\n");
extern void isr_usb(void);
extern void isr_sie(void);

static int dockKnown(void)
{
    static const uint8_t k[16] = {0x68, 0xfe, 0x7f, 0x04, 0xe2, 0x9e, 0xce, 0xff, 0xe0, 0x0e, 0xc1, 0x01, 0x50, 0xee, 0xe8, 0x08};
    const volatile uint8_t* p = (const volatile uint8_t*)0x0200134Cu;
    for (int i = 0; i < 16; i++) if (p[i] != k[i]) return 0;
    return 1;
}

static int32_t run(hb_probe_ctx_t* c)
{
    C = c;
    char b[48], *p;
    const uint32_t secs = p_arg32(c, 0, 20);
    C->host->fill(0, 0, 240, 240, 0);
    line(0, "USB IRQ PROBE", YELLOW);
    line(1, "Unplug the computer.", WHITE);
    line(2, "OTG adapter, hub, UCA202", WHITE);
    line(3, "timecode into its line in", WHITE);
    if (!dockKnown()) { line(5, "not fm1-dock 0.9.0 5d16166", RED); p_text(c, "error", "dock"); return 1; }
    for (uint32_t s = secs; s > 0; s--) {
        p = str(b, "host mode in "); p = dec(p, s); p = str(p, " s"); *p = 0;
        line(5, b, GREEN);
        for (int k = 0; k < 10; k++) { const uint32_t t = p_ticks(); while (p_ticks() - t < 2400000u) C->wdt_feed(); }
    }
    p_irq_off();
    *(volatile uint8_t*)(DOCK_USB + 0u) = 0;           // fm1-dock's USB off: its poll, uac_service, retry
    *(volatile uint8_t*)(DOCK_USB + 1u) = 0;
    *(volatile uint8_t*)(DOCK_USB + 64u) = 1;
    umsc_host(&kEnv);
    line(5, "host mode on: plug in (3 min)", GREEN);
    uint32_t t0 = p_ticks();
    for (uint32_t n = 0; !umsc_attached() && n < 18000u; n++) wait_us(10000);   // (3 min: no hurry; 180 s of ticks
                                                                                //  would pass 2^32)
    if (!umsc_attached()) { line(6, "no device", RED); p_text(c, "error", "no device"); goto done; }
    line(6, "enumerating (hubs: a few s)", WHITE);
    p_i32(c, "open", umsc_open());
    if (!umsc_audio.ep) { line(7, "no audio input found", RED); p_text(c, "error", "no audio"); goto done; }
    p_hex(c, "audio_id", umsc_audio.id);
    s_ch = umsc_audio.ch ? umsc_audio.ch : 2; s_sb = umsc_audio.bytes ? umsc_audio.bytes : 2;
    set_addr(umsc_audio.addr);
    s_maxp0 = umsc_audio.maxp0;
    {
        uint8_t si[8] = {0x01, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        si[2] = (uint8_t)umsc_audio.alt; si[4] = (uint8_t)umsc_audio.ifc;
        const int r = ctrl(si, 0, 0);
        p_i32(c, "set_interface", r);
        if (r) { line(7, "SET_INTERFACE failed", RED); goto done; }
    }
    line(7, "audio found: interrupts on", GREEN);
    sie_wr(S_INDEX, 3);
    sie_wr(S_RXMAXP, umsc_audio.maxp > 255 ? 0xFF : umsc_audio.maxp);
    sie_wr(S_RXTYPE, 0x10u | umsc_audio.ep);
    sie_wr(S_RXINTERVAL, 1);
    sie_wr(S_RXCSR2, 0); sie_wr(S_RXCSR1, 0x90);
    EP3_RADR = (uint32_t)(uintptr_t)s_arx;
    sie_wr(S_RXCSR1, 0x20);
    sie_wr(S_INDEX, 1);
    // the handler: vector 9, priority 5, the SIE's SOF interrupt on
    const uint32_t oldVec = VEC[IRQ_USB], sh = (IRQ_USB & 7u) * 4u, oldCfg = ICFG(IRQ_USB);
    sie_wr(7, 0); sie_wr(8, 0); sie_wr(9, 0); sie_wr(10, 0);   // INTRTX1E / 2E, INTRRX1E / 2E: the endpoints' off
    (void)sie_rd(2); (void)sie_rd(3); (void)sie_rd(4); (void)sie_rd(5); (void)sie_rd(S_INTRUSB);
    sie_wr(S_INTRUSBE, 0);
    sie_wr(9, 0x08);                                   // INTRRX1E: EP3 only
    CON0 |= CLR_SOF_PND;
    CON0 |= SOFIE | (1u << 11);                        // the SOF interrupt (IRQ 8) and the SIE's (IRQ 9: SIEIE)
    const uint32_t oldVec9 = VEC[9], oldCfg9 = ICFG(9);
    vec_set(9, (uint32_t)(uintptr_t)isr_sie);
    ICFG(9) = (oldCfg9 & ~(0xFu << 4)) | (((5u << 1) | 1u) << 4);
    vec_set(IRQ_USB, (uint32_t)(uintptr_t)isr_usb);
    ICFG(IRQ_USB) = (ICFG(IRQ_USB) & ~(0xFu << sh)) | (((5u << 1) | 1u) << sh);   // (fresh: 8 and 9 share the word)
    p_u32(c, "irq_on_next", 1);
    p_irq_on();
    {   // what the handler did in its first 10 ms and 100 ms (kept if what comes next crashes)
        const uint32_t ta = p_ticks();
        while (p_ticks() - ta < 240000u) ;
        uint32_t w[6] = {s_irqs, s_sofs, s_pk, s_miss, s_durMax / 24u, s_gapMax / 24u};
        p_words(c, "at10ms_irqs_sofs_pk_miss_durus_gapus", w, 6);
        while (p_ticks() - ta < 2400000u) C->wdt_feed();
        uint32_t w2[6] = {s_irqs, s_sofs, s_pk, s_miss, s_durMax / 24u, s_gapMax / 24u};
        p_words(c, "at100ms_irqs_sofs_pk_miss_durus_gapus", w2, 6);
        for (int k = 0; k < 4; k++) {                  // phase A: 2 s of the handler alone, the main loop idle
            while (p_ticks() - ta < 2400000u + 12000000u * (uint32_t)(k + 1)) C->wdt_feed();
            uint32_t w3[7] = {s_irqs, s_sofs, s_pk, s_miss, s_err, s_durMax / 24u, s_gapMax / 24u};
            p_words(c, "a_irqs_sofs_pk_miss_err_durus_gapus", w3, 7);
        }
        p_u32(c, "phase_b_next", 1);
        p_irq_off();                                   // (phase B's counts from here: the ring as it starts)
        s_rd = s_wr; s_pk = s_miss = s_err = s_over = s_pend = 0; s_sofs = 0; s_rxIrqs = 0; s_rearm = 0;
        p_irq_on();
    }

    static vinyl_scan sc;                              // both sides x every flag combination (fw/vinyl.c)
    vinyl_scan_begin(&sc, 48000);
    tc_dec* const dd = sc.dec;
    tc_dec* d = &dd[8];                                // (side B flags 0 until the scan picks)
    uint32_t scGood[2][16] = {{0}};                    // per packet mode: locked forwards, in all (ms)
    int32_t found[2] = {-1, -1};
    uint32_t speedMin[2] = {99999, 99999}, speedMax[2] = {0, 0};
    uint32_t foundFrames = 0;
    static int16_t cap[2][2 * 600];                    // per packet mode: 12.5 ms raw, from 1 s into it
    // how far each sample strays from a smooth sine (x[n+1] = 2cos(w) x[n] - x[n-1], w from the measured
    // speed), by its place in the 48-frame packet: a USB fault shows at one place, a noisy signal everywhere
    static uint32_t resid[2][2][48], residN[2][48];
    int32_t hL1 = 0, hL2 = 0, hR1 = 0, hR2 = 0;
    uint32_t ncap[2] = {0, 0}, modeFrames = 0;
    uint32_t best[16] = {0};
    static int16_t chunk[2 * 256];
    uint32_t sp[16], lk[16], frames = 0, mainPasses = 0;
    int32_t pos[16];
    uint32_t win1[16], win5[16], val1[16], val5[16];   // flags 1 and 5 (the ones that lock): window, valid bits
    t0 = p_ticks();
    for (int k = 0; k < 16; k++) {                     // 8 s, a line each half second; 4 s each packet mode
        if (k == 8) {                                  // the second half: mode 1, the scan afresh
            p_irq_off(); s_mode = 1; s_prevN = 0; p_irq_on();
            vinyl_scan_begin(&sc, 48000); d = &dd[8]; modeFrames = 0;
        }
        while (p_ticks() - t0 < 12000000u * (uint32_t)(k + 1)) {
            C->wdt_feed();
            mainPasses++;
            while (s_wr - s_rd >= 256u) {
                for (uint32_t i = 0; i < 256u; i++) {
                    const uint32_t q = (s_rd + i) & (RING - 1u);
                    chunk[2 * i] = s_ring[2 * q]; chunk[2 * i + 1] = s_ring[2 * q + 1];
                }
                s_rd += 256u;
                if (sc.found < 0) {
                    if (vinyl_scan_push(&sc, chunk, 256, (frames + 256u) / 48u * 1000u)) { foundFrames = frames + 256; d = &dd[sc.found]; }
                } else {
                    for (uint32_t f = 0; f < 16; f++) tc_push(&dd[f], chunk, 256);   // (all on: their lock lines)
                }
                const uint32_t md = s_mode;
                for (uint32_t f = 0; f < 16; f++) {
                    if (dd[f].valid > best[f]) best[f] = dd[f].valid;
                    if (dd[f].locked && dd[f].speed > 0.6f) scGood[md][f] += 256;
                }
                if (sc.found >= 0 && found[md] < 0) found[md] = sc.found;
                { const float sp0 = dd[sc.found >= 0 ? sc.found : 0].speed; const uint32_t a = (uint32_t)(int32_t)((sp0 < 0 ? -sp0 : sp0) * 1000.f);
                  if (modeFrames > 24000u) { if (a < speedMin[md]) speedMin[md] = a; if (a > speedMax[md]) speedMax[md] = a; } }
                modeFrames += 256;
                { const float sp0 = dd[sc.found >= 0 ? sc.found : 8].speed; const float w = 6.2831853f / 48.f * (sp0 < 0 ? -sp0 : sp0);
                  const float k2 = 2.f * (1.f - w * w / 2.f + w * w * w * w / 24.f);   // 2 cos w
                  for (uint32_t i = 0; i < 256u; i++) {
                      const int32_t L = chunk[2 * i], R = chunk[2 * i + 1];
                      const uint32_t pos = (frames + i) % 48u;
                      if (modeFrames > 24000u) {
                          int32_t eL = L - (int32_t)(k2 * (float)hL1) + hL2, eR = R - (int32_t)(k2 * (float)hR1) + hR2;
                          resid[md][0][pos] += (uint32_t)(eL < 0 ? -eL : eL); resid[md][1][pos] += (uint32_t)(eR < 0 ? -eR : eR);
                          residN[md][pos]++;
                      }
                      hL2 = hL1; hL1 = L; hR2 = hR1; hR1 = R;
                  } }
                if (modeFrames > 48000u) for (uint32_t i = 0; i < 256u && ncap[md] < 600u; i++, ncap[md]++) {
                    cap[md][2 * ncap[md]] = chunk[2 * i]; cap[md][2 * ncap[md] + 1] = chunk[2 * i + 1];
                }
                frames += 256;
            }
        }
        sp[k] = (uint32_t)(int32_t)(d->speed * 1000.f); lk[k] = d->locked | d->valid << 8; pos[k] = d->position;
        win1[k] = d->bitstream | (uint32_t)d->forwards << 31 | (uint32_t)d->locked << 30; val1[k] = d->valid;
        win5[k] = (uint32_t)sc.found; val5[k] = 0;
        { uint32_t w[8] = {s_irqs, s_sofs, s_pk, s_miss, s_err, frames, sp[k], lk[k]};
          p_words(c, "step_irqs_sofs_pk_miss_err_frames_speed_lock", w, 8); }
        p = str(b, "irq "); p = dec(p, s_sofs); p = str(p, " pk "); p = dec(p, s_pk); p = str(p, " miss "); p = dec(p, s_miss); *p = 0;
        line(8, b, s_miss ? YELLOW : GREEN);
        p = str(b, "speed "); p = dec(p, sp[k] & 0x80000000u ? 0 : sp[k]); p = str(p, d->forwards ? " fwd" : " rev");
        p = str(p, d->locked ? " LOCKED" : " -");
        if (sc.found >= 0) { p = str(p, sc.found >= 8 ? " side B f" : " side A f"); p = dec(p, (uint32_t)sc.found & 7u); }
        else p = str(p, " scanning");
        *p = 0;
        line(9, b, d->locked ? GREEN : WHITE);
    }
    p_irq_off();
    CON0 &= ~(SOFIE | (1u << 11));
    sie_wr(9, 0);
    ICFG(9) &= ~(0xFu << 4); vec_set(9, oldVec9); (void)oldCfg9;
    p_u32(c, "rx_irqs", s_rxIrqs); p_u32(c, "packet_max_bytes", s_nMax); p_u32(c, "rearms", s_rearm);
    ICFG(IRQ_USB) &= ~(0xFu << sh); (void)oldCfg; vec_set(IRQ_USB, oldVec);
    p_u32(c, "irqs", s_irqs); p_u32(c, "sofs", s_sofs); p_u32(c, "packets", s_pk); p_u32(c, "bytes", s_bytes);
    p_u32(c, "misses", s_miss); p_u32(c, "errors", s_err); p_hex(c, "last_err_csr", s_lastCsr);
    p_u32(c, "ring_overflows", s_over); p_u32(c, "still_pending", s_pend); p_hex(c, "intrusb_seen", s_intr);
    p_u32(c, "gap_max_us", s_gapMax / 24u); p_u32(c, "gaps_over_1200us", s_gapOver);
    p_u32(c, "handler_max_us", s_durMax / 24u);
    p_u32(c, "frames_decoded", frames); p_u32(c, "main_passes", mainPasses);
    p_words(c, "best_valid_A0_7_B0_7", best, 16);
    for (int m = 0; m < 2; m++) for (int f = 0; f < 16; f++) scGood[m][f] /= 48u;
    p_words(c, "mode0_pingpong_fwd_locked_ms_A0_7_B0_7", scGood[0], 16);
    p_words(c, "mode1_wait_fwd_locked_ms_A0_7_B0_7", scGood[1], 16);
    p_i32(c, "mode0_found", found[0]); p_i32(c, "mode1_found", found[1]);
    p_u32(c, "mode0_speed_min_x1000", speedMin[0]); p_u32(c, "mode0_speed_max_x1000", speedMax[0]);
    p_u32(c, "mode1_speed_min_x1000", speedMin[1]); p_u32(c, "mode1_speed_max_x1000", speedMax[1]);
    p_bytes(c, "mode0_capture_LR16", cap[0], 4u * ncap[0]); p_bytes(c, "mode1_capture_LR16", cap[1], 4u * ncap[1]);
    for (int m = 0; m < 2; m++) for (int ch = 0; ch < 2; ch++) for (int q = 0; q < 48; q++) resid[m][ch][q] = residN[m][q] ? resid[m][ch][q] / residN[m][q] : 0;
    p_words(c, "mode0_residL_by_pos", resid[0][0], 48); p_words(c, "mode0_residR_by_pos", resid[0][1], 48);
    p_words(c, "mode1_residL_by_pos", resid[1][0], 48); p_words(c, "mode1_residR_by_pos", resid[1][1], 48);
    p_i32(c, "scan_found_index", sc.found); p_u32(c, "scan_side_is_B", sc.found >= 8); p_u32(c, "scan_flags", vinyl_scan_flags(&sc));
    p_u32(c, "scan_found_at_ms", foundFrames / 48u);
    { uint32_t lk8[16], sp8[16];
      for (uint32_t f = 0; f < 16; f++) { lk8[f] = dd[f].locked; sp8[f] = (uint32_t)(int32_t)(dd[f].speed * 1000.f); }
      p_words(c, "locked_A0_7_B0_7", lk8, 16); p_words(c, "speed_x1000_A0_7_B0_7", sp8, 16); }
    p_words(c, "pick_win_fwd31_lock30", win1, 16); p_words(c, "pick_valid", val1, 16);
    p_words(c, "found_index_by_step", win5, 16); (void)val5;
    p_words(c, "speed_x1000", sp, 16); p_words(c, "locked_valid", lk, 16); p_words(c, "position", (uint32_t*)pos, 16);
    p = str(b, "gap max "); p = dec(p, s_gapMax / 24u); p = str(p, " us, isr "); p = dec(p, s_durMax / 24u); p = str(p, " us"); *p = 0;
    line(10, b, WHITE);
    (void)hex;
done:
    p_irq_off();
    umsc_close();
    line(12, "Plug the computer back in. 8 s", YELLOW);
    for (;;) ;
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "USBIRQ", run};
