// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// USBAUDIO: an fm1-dock probe (tools/probe.py; runs once from RAM, never touches the flash): a USB audio
// interface's line in (a Behringer UCA202) on the FM-1's own port, through a hub, read for a second: the first
// step towards DVS (timecode vinyl). A countdown (move the FM-1 to the powered OTG adapter, the hub on it,
// the UCA202 on the hub, timecode or any music playing into its line in), usbmsc.c's host mode and hub
// enumeration (a stick, a DJ2Go there too are fine), then the audio device: SET_INTERFACE to its streaming
// alternate setting, SET_CUR of the sampling rate (48 kHz), and its isochronous IN endpoint on the
// controller's EP3, one packet a frame for 1000 frames. Shown and kept: the packets that came, their sizes,
// errors, each channel's min / max and the rising zero crossings (a 1 kHz carrier: ~1000), the first samples.
// Then the watchdog restarts the FM-1; with the computer back: tools/probe.py result.
//
//   cd ~/sandbox/thirdparty/fm1-dock && uv run --with python-rtmidi tools/probe.py run \
//       ~/sandbox/audiodestrukt/freedj-nano/ch32v003/fm1/probes/usbaudio.c --on fm1 --args 20 --ms 90000
#include "probe.h"
#include "../usbmsc.c"

#define EP3_RADR (*(volatile uint32_t *)0x11830u)              // (EP_RADR(n) = 0x11820 + 8 (n - 1): fm1-dock's fm1_usb.h)

static hb_probe_ctx_t* C;
static uint32_t pticks(void) { return p_ticks(); }
static void pidle(void) { C->wdt_feed(); }
static const umsc_env kEnv = {pticks, pidle};

static const char kHex[] = "0123456789ABCDEF";
static char* hex(char* p, uint32_t v, int digits) { while (digits--) *p++ = kHex[(v >> (4 * digits)) & 15]; return p; }
static char* dec(char* p, uint32_t v) { char t[10]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) *p++ = t[--n]; return p; }
static char* sdec(char* p, int32_t v) { if (v < 0) { *p++ = '-'; v = -v; } return dec(p, (uint32_t)v); }
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

// a control transfer with OUT data (n <= EP0's packet): setup, one data packet, an IN status
static int ctrl_out(const uint8_t setup[8], const uint8_t* data, uint32_t n)
{
    sie_wr(S_INDEX, 0);
    for (int i = 0; i < 8; i++) s_ep0[i] = setup[i];
    __asm__ volatile("csync" ::: "memory");
    EP0_ADR = (uint32_t)(uintptr_t)s_ep0;
    EP0_CNT = 8;
    wait_us(50);
    sie_wr(S_CSR0, 0x0A);                                      // SetupPkt | TxPktRdy
    int r = ep0_wait(0x02, 0x00);
    if (r) return r * 10 - 1;
    for (uint32_t i = 0; i < n; i++) s_ep0[i] = data[i];
    __asm__ volatile("csync" ::: "memory");
    EP0_ADR = (uint32_t)(uintptr_t)s_ep0;
    EP0_CNT = n;
    wait_us(50);
    sie_wr(S_CSR0, 0x02);                                      // TxPktRdy: the data
    r = ep0_wait(0x02, 0x00);
    if (r) return r * 10 - 2;
    sie_wr(S_CSR0, 0x60);                                      // status: an IN with no data
    r = ep0_wait(0x01, 0x01);
    sie_wr(S_CSR0, 0x00);
    return r ? r * 10 - 3 : 0;
}

static uint8_t s_arx[1024 + 8] __attribute__((aligned(8)));

static int32_t run(hb_probe_ctx_t* c)
{
    C = c;
    char b[48], *p;
    const uint32_t secs = p_arg32(c, 0, 20);
    C->host->fill(0, 0, 240, 240, 0);
    line(0, "USB AUDIO PROBE", YELLOW);
    line(1, "Unplug the computer.", WHITE);
    line(2, "OTG adapter, hub, UCA202", WHITE);
    line(3, "timecode into its line in", WHITE);
    for (uint32_t s = secs; s > 0; s--) {
        p = str(b, "host mode in "); p = dec(p, s); p = str(p, " s"); *p = 0;
        line(5, b, GREEN);
        for (int k = 0; k < 10; k++) { const uint32_t t = p_ticks(); while (p_ticks() - t < 2400000u) C->wdt_feed(); }
    }
    p_irq_off();
    umsc_host(&kEnv);
    line(5, "host mode on: waiting", GREEN);
    uint32_t t0 = p_ticks();
    while (!umsc_attached() && p_ticks() - t0 < 24000000u * 20u) wait_us(10000);
    if (!umsc_attached()) { line(6, "no device", RED); p_text(c, "error", "no device"); goto done; }
    line(6, "enumerating (hubs: a few s)", WHITE);
    {
        const int ro = umsc_open();
        p_i32(c, "open", ro);
        p_u32(c, "open_err", umsc_err);
        p_hex(c, "hub_id", umsc_hubid);
        p_hex(c, "midi_id", umsc_midiid);
        p_words(c, "trace", umsc_trace, umsc_ntrace);
    }
    if (!umsc_audio.ep) { line(7, "no audio input found", RED); p_text(c, "error", "no audio"); goto done; }
    {
        const umsc_audio_t* a = &umsc_audio;
        const uint32_t w[11] = {a->addr, a->maxp0, a->ifc, a->alt, a->ep, a->maxp, a->ch, a->bytes, a->bits, a->rate, a->id};
        p_words(c, "audio_addr_maxp0_ifc_alt_ep_maxp_ch_bytes_bits_rate_id", w, 11);
        p = str(b, "audio "); p = hex(p, a->id >> 16, 4); p = str(p, ":"); p = hex(p, a->id & 0xFFFF, 4);
        p = str(p, " "); p = dec(p, a->ch); p = str(p, "ch "); p = dec(p, a->bits); p = str(p, "b"); *p = 0;
        line(7, b, GREEN);
    }
    set_addr(umsc_audio.addr);
    s_maxp0 = umsc_audio.maxp0;
    {
        uint8_t si[8] = {0x01, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};   // SET_INTERFACE(ifc, alt)
        si[2] = (uint8_t)umsc_audio.alt; si[4] = (uint8_t)umsc_audio.ifc;
        const int r = ctrl(si, 0, 0);
        p_i32(c, "set_interface", r);
        if (r) { line(8, "SET_INTERFACE failed", RED); goto done; }
        uint8_t sc[8] = {0x22, 0x01, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00};    // SET_CUR(SAMPLING_FREQ) to the endpoint
        sc[4] = (uint8_t)(0x80u | umsc_audio.ep);
        static const uint8_t k48[3] = {0x80, 0xBB, 0x00};                   // 48000
        const int r2 = ctrl_out(sc, k48, 3);
        p_i32(c, "set_rate_48k", r2);
        uint8_t gc[8] = {0xA2, 0x81, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00};    // GET_CUR(SAMPLING_FREQ)
        gc[4] = (uint8_t)(0x80u | umsc_audio.ep);
        uint8_t f[4] = {0}; uint32_t got = 0;
        const int r3 = ctrl(gc, f, &got);
        p_i32(c, "get_rate", r3);
        p_u32(c, "rate_now", f[0] | (uint32_t)f[1] << 8 | (uint32_t)f[2] << 16);
        p = str(b, "rate set "); p = str(p, r2 ? "failed" : "ok"); p = str(p, ", now ");
        p = dec(p, f[0] | (uint32_t)f[1] << 8 | (uint32_t)f[2] << 16); *p = 0;
        line(8, b, r2 ? YELLOW : GREEN);
    }

    // EP3: the isochronous IN, one packet a frame
    sie_wr(S_INDEX, 3);
    sie_wr(S_RXMAXP, umsc_audio.maxp > 255 ? 0xFF : umsc_audio.maxp);
    sie_wr(S_RXTYPE, 0x10u | umsc_audio.ep);                   // isochronous
    sie_wr(S_RXINTERVAL, 1);                                   // every frame
    sie_wr(S_RXCSR2, 0); sie_wr(S_RXCSR1, 0x90);
    line(9, "reading 1000 frames", WHITE);
    {
        enum { N = 1000 };
        uint32_t pk = 0, bytes = 0, tmo = 0, err = 0, csr = 0, sizes[16], ns = 0, first[64], nf = 0;
        int32_t mn[2] = {32767, 32767}, mx[2] = {-32768, -32768};
        uint32_t cross[2] = {0, 0};
        int8_t side[2] = {0, 0};
        const uint32_t sb = umsc_audio.bytes ? umsc_audio.bytes : 2, nch = umsc_audio.ch ? umsc_audio.ch : 2;
        const uint32_t f0 = sie_rd(S_FRAME1) | sie_rd(S_FRAME2) << 8;
        const uint32_t ta = p_ticks();
        for (uint32_t k = 0; k < N; k++) {
            sie_wr(S_INDEX, 3);
            EP3_RADR = (uint32_t)(uintptr_t)s_arx;
            sie_wr(S_RXCSR1, 0x20);                            // ReqPkt: an IN in the next frame
            uint32_t v = 0, ok = 0;
            for (uint32_t t = p_ticks(); p_ticks() - t < 24000u * 3u;) {
                v = sie_rd(S_RXCSR1);
                if (v & 0x01u) { ok = 1; break; }
                if (v & 0x4Cu) break;
            }
            if (!ok) {
                if (v & 0x4Cu) { err++; csr = v; } else tmo++;
                sie_wr(S_RXCSR1, 0); sie_wr(S_RXCSR1, 0x10);
                continue;
            }
            const uint32_t n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) & 7u) << 8;
            wait_us(30);
            __asm__ volatile("ssync" ::: "memory");
            if (v & 0x0Cu) { err++; csr = v; }                 // (iso: a CRC error still delivers)
            pk++; bytes += n;
            if (ns < 16) sizes[ns++] = n;
            for (uint32_t i = 0; i + sb * nch <= n && i + sb * nch <= sizeof s_arx; i += sb * nch) {
                for (uint32_t ch = 0; ch < 2 && ch < nch; ch++) {
                    const uint8_t* q = s_arx + i + ch * sb;
                    const int32_t x = sb == 2 ? (int16_t)(q[0] | q[1] << 8) : sb == 3 ? (int16_t)(q[1] | q[2] << 8) : (int8_t)q[0] * 256;
                    if (x < mn[ch]) mn[ch] = x;
                    if (x > mx[ch]) mx[ch] = x;
                    if (x > 300 && side[ch] <= 0) { if (side[ch] < 0) cross[ch]++; side[ch] = 1; }
                    else if (x < -300) side[ch] = -1;
                }
                if (nf < 64 && k >= 100) {                     // (some way in: past any start-up silence)
                    const uint8_t* q = s_arx + i;
                    first[nf++] = sb == 2 ? (uint32_t)(q[0] | q[1] << 8) | (uint32_t)(q[sb] | q[sb + 1] << 8) << 16 : 0;
                }
            }
            sie_wr(S_RXCSR1, 0x10);                            // FlushFIFO
        }
        const uint32_t us = (p_ticks() - ta) / 24u;
        const uint32_t f1 = sie_rd(S_FRAME1) | sie_rd(S_FRAME2) << 8;
        p_u32(c, "packets", pk); p_u32(c, "bytes", bytes); p_u32(c, "timeouts", tmo); p_u32(c, "errors", err);
        p_hex(c, "last_err_csr", csr);
        p_u32(c, "us", us); p_u32(c, "frames", (f1 - f0) & 0x7FFu);
        p_words(c, "sizes", sizes, ns);
        const uint32_t mm[4] = {(uint32_t)mn[0], (uint32_t)mx[0], (uint32_t)mn[1], (uint32_t)mx[1]};
        p_words(c, "min_max_L_R", mm, 4);
        p_words(c, "rising_crossings_L_R", cross, 2);
        p_words(c, "samples_L_R16", first, nf);
        p = str(b, "packets "); p = dec(p, pk); p = str(p, " bytes/pk "); p = dec(p, pk ? bytes / pk : 0); *p = 0;
        line(9, b, pk > 900 ? GREEN : pk ? YELLOW : RED);
        p = str(b, "L "); p = sdec(p, mn[0]); p = str(p, ".."); p = sdec(p, mx[0]); p = str(p, " x"); p = dec(p, cross[0]); *p = 0;
        line(10, b, WHITE);
        p = str(b, "R "); p = sdec(p, mn[1]); p = str(p, ".."); p = sdec(p, mx[1]); p = str(p, " x"); p = dec(p, cross[1]); *p = 0;
        line(11, b, WHITE);
        p = str(b, "errors "); p = dec(p, err); p = str(p, " timeouts "); p = dec(p, tmo); *p = 0;
        line(12, b, err || tmo ? YELLOW : GREEN);
    }
done:
    umsc_close();
    line(13, "Plug the computer back in. 8 s", YELLOW);
    for (;;) ;
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "USBAUDIO", run};
