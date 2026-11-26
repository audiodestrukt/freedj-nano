// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// USBHOST: an fm1-dock probe (tools/probe.py; runs once from RAM, never touches the flash). Can the FM-1's
// USB-C port be a USB host? It counts down on the screen (move the FM-1 from the computer to the adapter,
// the stick and the power in it), puts the USB controller in host mode as JieLi's AC79 SDK does
// (usb_sie_close, usb_h_sie_init, usb_h_chirp_and_reset, usb_ctrl_transfer: cpu/wl80/liba/cpu.a), waits
// for a device, resets it and asks for its device descriptor (VID / PID), shows what it got, then lets
// the watchdog restart the FM-1 (USB back as a device, as at power-on). After the restart, with the
// computer plugged in again: tools/probe.py result.
//
//   cd ~/sandbox/thirdparty/fm1-dock && uv run --with python-rtmidi tools/probe.py run \
//       ~/sandbox/audiodestrukt/freedj-nano/ch32v003/fm1/probes/usbhost.c --on fm1
//
// Registers (fm1-dock firmware/hal/fm1_usb.h; bits from the SDK's include_lib/driver/cpu/wl82/asm/usb.h):
//   CON0 0x11800: 0 PHY_ON, 1 LOW_SPEED, 2 USB_NRST, 3 TM1, 4 CID (1 = device), 5 VBUS, 16 DM_SE, 17 DP_SE
//   CON1 0x11804: the SIE mailbox. IO_CON0 0x51000: 4 DPPD, 5 DMPD, 6 DPPU, 7 DMPU, 8 IO_PU_MODE,
//   9 DPDIE, 10 DMDIE, 11 IO_MODE, 12 SR. SIE (MUSB): FADDR 0, POWER 1, INDEX 14, DEVCTL 15, CSR0 17, COUNT0 22.
#include "probe.h"

#define CON0    (*(volatile uint32_t *)0x11800u)
#define CON1    (*(volatile uint32_t *)0x11804u)
#define EP0_CNT (*(volatile uint32_t *)0x11808u)
#define EP0_ADR (*(volatile uint32_t *)0x11818u)
#define IO_CON0 (*(volatile uint32_t *)0x51000u)
#define IO_CON1 (*(volatile uint32_t *)0x51004u)
enum { S_FADDR = 0, S_POWER = 1, S_INTRUSB = 6, S_INTRUSBE = 11, S_FRAME1 = 12, S_FRAME2 = 13, S_INDEX = 14, S_DEVCTL = 15,
       S_CSR0 = 17, S_COUNT0 = 22 };
static uint32_t s_fifo8;                          // COUNT0 once the setup bytes were handed over (8: the DMA read them)

static hb_probe_ctx_t* C;
static uint32_t s_timeouts;

static void wait_us(uint32_t us)
{
    for (uint32_t t = p_ticks(); p_ticks() - t < 24u * us;) C->wdt_feed();
}
static void sie_wr(uint32_t r, uint32_t v)
{
    CON1 = ((r << 8) & 0xFFFF00u) | (v & 0xFFu);
    __asm__ volatile("csync" ::: "memory");
    uint32_t n = 20000;
    while (!(CON1 & 0x8000u) && --n) ;
    if (!n) s_timeouts++;
}
static uint32_t sie_rd(uint32_t r)
{
    CON1 = ((r << 8) & 0xFFBF00u) | 0x4000u;
    __asm__ volatile("csync" ::: "memory");
    uint32_t n = 20000;
    while (!(CON1 & 0x8000u)) if (!--n) { s_timeouts++; return 0xFFFFu; }
    return CON1 & 0xFFu;
}

// the screen (the host's fill / text: polled SPI, fine with the interrupts off)
static const char kHex[] = "0123456789ABCDEF";
static char* hex(char* p, uint32_t v, int digits) { while (digits--) *p++ = kHex[(v >> (4 * digits)) & 15]; return p; }
static char* dec(char* p, uint32_t v) { char t[10]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) *p++ = t[--n]; return p; }
static char* str(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static void line(int row, const char* s, uint16_t c)
{
    C->host->fill(0, 4 + row * 18, 240, 18, 0);              // 13 rows of 18 px
    C->host->text(4, 4 + row * 18, 232, 0, s, c, 0);
}
#define WHITE  HB_RGB(235, 235, 235)
#define YELLOW HB_RGB(255, 210, 0)
#define GREEN  HB_RGB(80, 220, 110)
#define RED    HB_RGB(255, 70, 50)

// EP0 in host mode (the SDK's usb_ctrl_transfer): 0 done, < 0 a stall / an error / no answer
static int ep0_wait(uint32_t mask, uint32_t want, uint32_t* csr)
{
    for (uint32_t t = p_ticks(); p_ticks() - t < 24000u * 200u;) {
        sie_wr(S_INDEX, 0);
        const uint32_t v = sie_rd(S_CSR0);
        *csr = v;
        if (v == 0xFFFFu) return -3;
        if (v & 0x04u) { sie_wr(S_CSR0, 0); return -1; }       // RxStall
        if (v & 0x10u) { sie_wr(S_CSR0, 0); return -2; }       // Error (no answer, 3 tries)
        if ((v & mask) == want) return 0;
        C->wdt_feed();
    }
    return -4;                                                 // time out
}
static int getDesc(uint8_t* ep0, uint8_t* out, uint32_t want, uint32_t maxp, uint32_t* got, uint32_t* csr)
{
    static const uint8_t setup[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00};   // GET_DESCRIPTOR(device)
    uint8_t s[8];
    for (int i = 0; i < 8; i++) s[i] = setup[i];
    s[6] = (uint8_t)want;
    sie_wr(S_FADDR, 0);
    sie_wr(S_INDEX, 0);
    for (int i = 0; i < 8; i++) ep0[i] = s[i];
    __asm__ volatile("csync" ::: "memory");
    EP0_ADR = (uint32_t)(uintptr_t)ep0;
    EP0_CNT = 8;
    wait_us(50);
    s_fifo8 = sie_rd(S_COUNT0);
    sie_wr(S_CSR0, 0x0A);                                      // SetupPkt | TxPktRdy
    int r = ep0_wait(0x02, 0x00, csr);
    if (r) return r * 10 - 1;                                  // -11.. : the setup stage
    *got = 0;
    while (*got < want) {
        sie_wr(S_CSR0, 0x20);                                  // ReqPkt
        r = ep0_wait(0x01, 0x01, csr);
        if (r) return r * 10 - 2;                              // -12.. : the data stage
        const uint32_t n = sie_rd(S_COUNT0);
        __asm__ volatile("ssync" ::: "memory");
        for (uint32_t i = 0; i < n && *got < 64; i++) out[(*got)++] = ep0[i];
        sie_wr(S_CSR0, 0x00);                                  // (RxPktRdy cleared)
        if (n < maxp || n == 0) break;
    }
    sie_wr(S_CSR0, 0x42);                                      // the status stage: StatusPkt | TxPktRdy
    ep0_wait(0x02, 0x00, csr);
    sie_wr(S_CSR0, 0x00);
    return 0;
}

static int32_t run(hb_probe_ctx_t* c)
{
    C = c;
    char b[48], *p;
    const uint32_t secs = p_arg32(c, 0, 20), watch = p_arg32(c, 1, 20);   // (--args countdown,watch: s)
    static uint8_t s_ep0[64 + 8] __attribute__((aligned(8)));   // EP0's DMA buffer (EP0_ADR reads back 0: write-only,
    uint8_t* const ep0 = s_ep0;                                //  so not fm1-dock's; the probe's RAM is the same SRAM)
    p_hex(c, "ep0_buf", (uint32_t)(uintptr_t)ep0);
    p_hex(c, "con0_device", CON0);
    p_hex(c, "io_con0_device", IO_CON0);

    C->host->fill(0, 0, 240, 240, 0);
    line(0, "USB HOST PROBE", YELLOW);
    line(1, "Unplug the computer.", WHITE);
    line(2, "Plug in the adapter,", WHITE);
    line(3, "its charger, the stick.", WHITE);
    for (uint32_t s = secs; s > 0; s--) {                      // (the interrupts on: fm1-dock's USB as it was)
        p = str(b, "host mode in "); p = dec(p, s); p = str(p, " s"); *p = 0;
        line(5, b, GREEN);
        wait_us(1000000);
    }

    p_irq_off();                                               // fm1-dock's USB poll (TIMER5) and audio stop here
    CON0 = 0; CON1 = 0; IO_CON0 = 0x0Cu;                       // usb_sie_close
    wait_us(20000);
    IO_CON0 = 0; IO_CON1 = 0;                                  // usb_h_sie_init
    wait_us(1000);
    IO_CON0 |= (1u << 9) | (1u << 10);                         // DPDIE, DMDIE
    IO_CON0 &= ~((1u << 6) | (1u << 7));                       // no pull-ups
    IO_CON0 |= (1u << 4) | (1u << 5);                          // the host's pull-downs on D+ and D-
    IO_CON0 &= ~((1u << 8) | (1u << 11));
    CON0 |= 0x25u;                                             // PHY_ON | USB_NRST | VBUS (valid); CID 0: the A side
    wait_us(1000);
    sie_wr(S_INTRUSBE, 0);
    const uint32_t dev0 = sie_rd(S_DEVCTL);
    p_hex(c, "con0_host", CON0);
    p_hex(c, "io_con0_host", IO_CON0);
    p_hex(c, "devctl_after_init", dev0);
    p_u32(c, "sie_timeouts_init", s_timeouts);
    line(5, s_timeouts ? "SIE: no answer" : "host mode on", s_timeouts ? RED : GREEN);

    // a device: the session started at once (DEVCTL Session | HostReq, as usb_h_chirp_and_reset); then, a
    // millisecond at a time, D+ / D- as CON0 has them (the SDK's usb_hotplug_detect) and DEVCTL (the core's
    // own: HostMode + FSDev / LSDev once it sees a device). Every change is kept (up to 24), and the first
    // 3 s every 100 ms, to read after: CON0, IO_CON0, DEVCTL.
    sie_wr(S_DEVCTL, 0x03);
    uint32_t seen = 0, steady = 0, t0 = p_ticks(), last = 0xFFFFFFFFu, dev = 0, ms = 0;
    uint32_t chg[24 * 4], nchg = 0, smp[30 * 3], nsmp = 0;
    while (p_ticks() - t0 < 24000000u * watch) {
        const uint32_t c0 = CON0, io = IO_CON0;
        dev = sie_rd(S_DEVCTL);
        if (!(dev & 1u) && ms % 100 == 0) sie_wr(S_DEVCTL, 0x03);    // the session dropped: again
        const uint32_t se = c0 >> 16 & 3u;                           // bit 0 DM, bit 1 DP
        const uint32_t key = se | dev << 8 | (io & 0xFFFF0000u);
        if (key != last) {
            last = key;
            if (nchg < 24) { chg[4 * nchg] = ms; chg[4 * nchg + 1] = c0; chg[4 * nchg + 2] = io; chg[4 * nchg + 3] = dev; nchg++; }
            p = str(b, "D+ "); *p++ = (char)('0' + (se >> 1)); p = str(p, " D- "); *p++ = (char)('0' + (se & 1));
            p = str(p, " DEVCTL "); p = hex(p, dev, 2); *p = 0;
            line(6, b, se || (dev & 0x60u) ? GREEN : WHITE);
        }
        if (ms % 100 == 0 && nsmp < 30) { smp[3 * nsmp] = c0; smp[3 * nsmp + 1] = io; smp[3 * nsmp + 2] = dev; nsmp++; }
        steady = (se || ((dev & 0x04u) && (dev & 0x60u))) ? steady + 1 : 0;
        if (steady >= 50) { seen = se ? se : (dev & 0x40u) ? 2u : 1u; break; }
        if (ms % 500 == 0) {
            p = str(b, "waiting for a device "); p = dec(p, watch - (p_ticks() - t0) / 24000000u); *p = 0;
            line(7, b, WHITE);
        }
        wait_us(1000);
        ms++;
    }
    p_words(c, "changes_ms_con0_io_devctl", chg, 4 * nchg);
    p_words(c, "every_100ms_con0_io_devctl", smp, 3 * nsmp);
    p_u32(c, "lines", seen);                                   // 2: full speed, 1: low speed, 0: none
    p_hex(c, "devctl_last", dev);
    if (!seen) {
        line(7, "no device seen", RED);
    } else {
        line(7, seen == 2 ? "a full-speed device" : "a low-speed device", GREEN);
        for (int i = 0; i < 200; i++) { dev = sie_rd(S_DEVCTL); if (dev & 0x04u) break; wait_us(1000); }
        p_hex(c, "devctl_session", dev);
        if (dev & 0x20u && !(dev & 0x40u)) CON0 |= 2u;         // LSDev: low speed
        const uint32_t pw = sie_rd(S_POWER);
        sie_wr(S_POWER, pw | 8u);                              // the bus reset: 20 ms
        wait_us(20000);
        sie_wr(S_POWER, sie_rd(S_POWER) & ~8u);
        wait_us(20000);
        p_hex(c, "power_after_reset", sie_rd(S_POWER));
        p = str(b, "DEVCTL "); p = hex(p, dev, 2); p = str(p, (dev & 0x04u) ? " host" : " not host"); *p = 0;
        line(8, b, (dev & 0x04u) ? GREEN : RED);

        uint8_t d[64];
        uint32_t got = 0, csr = 0;
        // the core never saw the device (no FSDev, no Connect, no SOFs) and its VBUS level stays 01 whatever the
        // pin has: VBUS comes from CON0's bits. Each candidate in turn (bit 3 is in the device setup, 0x3C; 6, 7 are
        // unnamed), the session off and on: DEVCTL, INTRUSB, the frame counter's move in 20 ms. The first that
        // shows FS / LS or moving frames is kept.
        {
            static const uint32_t extra[6] = {0x00, 0x08, 0x40, 0x80, 0x48, 0xC8};
            uint32_t res[6 * 4], kept = 0xFF;
            for (uint32_t k = 0; k < 6; k++) {
                CON0 = (CON0 & ~0x1C8u) | 0x25u | extra[k];        // (bits 3, 6, 7, 8 cleared, then the candidate)
                sie_wr(S_DEVCTL, 0x00); wait_us(50000);
                sie_wr(S_DEVCTL, 0x03); wait_us(100000);
                const uint32_t g0 = sie_rd(S_FRAME1) | sie_rd(S_FRAME2) << 8;
                wait_us(20000);
                const uint32_t mv = ((sie_rd(S_FRAME1) | sie_rd(S_FRAME2) << 8) - g0) & 0x7FFu;
                const uint32_t dv = sie_rd(S_DEVCTL), iu = sie_rd(S_INTRUSB);
                res[4 * k] = extra[k]; res[4 * k + 1] = dv; res[4 * k + 2] = iu; res[4 * k + 3] = mv;
                p = str(b, "CON0+"); p = hex(p, extra[k], 2); p = str(p, " DEV "); p = hex(p, dv, 2);
                p = str(p, " F "); p = dec(p, mv); *p = 0;
                line(9 + (int)(k / 2), b, (dv & 0x60u) || mv ? GREEN : WHITE);
                if ((dv & 0x60u) || mv) { kept = k; break; }
            }
            p_words(c, "try_extra_devctl_intrusb_frames", res, 4 * (kept == 0xFF ? 6 : kept + 1));
            if (kept == 0xFF) CON0 = (CON0 & ~0x1C8u) | 0x25u;
            p_hex(c, "con0_kept", CON0);
        }
        // the bus: SOFs going (the frame counter moving), the core's own view (INTRUSB: 0x10 connect,
        // 0x20 disconnect, 0x80 VBUS error)
        const uint32_t f0 = sie_rd(S_FRAME1) | sie_rd(S_FRAME2) << 8;
        wait_us(5000);
        const uint32_t f1 = sie_rd(S_FRAME1) | sie_rd(S_FRAME2) << 8;
        p_u32(c, "frame_a", f0); p_u32(c, "frame_b_5ms_later", f1);
        p_hex(c, "intrusb", sie_rd(S_INTRUSB));
        p_hex(c, "devctl_before_setup", sie_rd(S_DEVCTL));
        int r = getDesc(ep0, d, 8, 8, &got, &csr);             // 8 first: bMaxPacketSize0
        p_u32(c, "count0_after_setup_dma", s_fifo8);
        p_i32(c, "try1", r); p_hex(c, "try1_csr0", csr);
        if (r) {                                               // again: the session off and on with the device there
            sie_wr(S_DEVCTL, 0x00); wait_us(100000);
            sie_wr(S_DEVCTL, 0x03); wait_us(200000);
            p_hex(c, "devctl_resession", sie_rd(S_DEVCTL));
            p_hex(c, "intrusb_resession", sie_rd(S_INTRUSB));
            sie_wr(S_POWER, sie_rd(S_POWER) | 8u); wait_us(50000);
            sie_wr(S_POWER, sie_rd(S_POWER) & ~8u); wait_us(50000);
            const uint32_t g0 = sie_rd(S_FRAME1) | sie_rd(S_FRAME2) << 8;
            wait_us(5000);
            p_u32(c, "frame_moved_5ms_resession", ((sie_rd(S_FRAME1) | sie_rd(S_FRAME2) << 8) - g0) & 0x7FF);
            r = getDesc(ep0, d, 8, 8, &got, &csr);
            p_i32(c, "try2", r); p_hex(c, "try2_csr0", csr);
        }
        uint32_t maxp = (r == 0 && got >= 8 && d[7]) ? d[7] : 8;
        if (r == 0) r = getDesc(ep0, d, 18, maxp, &got, &csr);
        p_i32(c, "get_descriptor", r);
        p_hex(c, "csr0_last", csr);
        p_bytes(c, "descriptor", d, got);
        p_u32(c, "sie_timeouts", s_timeouts);
        if (r == 0 && got >= 12) {
            const uint32_t vid = d[8] | (uint32_t)d[9] << 8, pid = d[10] | (uint32_t)d[11] << 8;
            p_hex(c, "vid", vid); p_hex(c, "pid", pid);
            p = str(b, "VID "); p = hex(p, vid, 4); p = str(p, " PID "); p = hex(p, pid, 4); *p = 0;
            line(9, b, GREEN);
            p = str(b, "class "); p = hex(p, d[4], 2); p = str(p, (d[4] == 9) ? " (a hub)" : d[4] == 0 ? " (per interface)" : ""); *p = 0;
            line(10, b, WHITE);
        } else {
            p = str(b, "descriptor: error "); p = dec(p, (uint32_t)(-r)); p = str(p, " csr "); p = hex(p, csr, 2); *p = 0;
            line(9, b, RED);
        }
    }

    CON0 = 0; CON1 = 0; IO_CON0 = 0x0Cu;                       // the port let go (usb_sie_close)
    line(11, "Plug the computer back", YELLOW);
    line(12, "in. Restarting in 8 s.", YELLOW);
    p_text(c, "done", "the watchdog restarts the FM-1");
    for (;;) ;                                                 // no more feeding: the watchdog restarts it (8 s)
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "USBHOST", run};
