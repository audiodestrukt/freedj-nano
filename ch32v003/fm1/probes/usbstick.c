// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// USBSTICK: an fm1-dock probe (tools/probe.py; runs once from RAM, never touches the flash): a USB stick on
// the FM-1's own port, read as a drive. A countdown (move the FM-1 from the computer to a powered, hub-less
// OTG adapter with the stick in it), host mode (usbhost.c's, with CON0 bit 3, which the bus needs to start),
// the stick enumerated (address 1, its configuration, its mass-storage interface and bulk endpoints), then
// Bulk-Only Transport: INQUIRY, READ CAPACITY(10), READ(10) of block 0. It shows what it got and lets the
// watchdog restart the FM-1; after, with the computer back: tools/probe.py result.
//
//   cd ~/sandbox/thirdparty/fm1-dock && uv run --with python-rtmidi tools/probe.py run \
//       ~/sandbox/audiodestrukt/freedj-nano/ch32v003/fm1/probes/usbstick.c --on fm1 --args 20 --ms 90000
//
// Registers as usbhost.c. EP1's FIFOs both ways (TX to the stick's bulk OUT, RX from its bulk IN), by DMA
// from / to buffers here (EP_TADR(1) 0x1181C, EP_RADR(1) 0x11820, EP_CNT(1) 0x1180C starts a TX), the
// indexed MUSB registers: TXMAXP 16, TXCSR1 17, TXCSR2 18, RXMAXP 19, RXCSR1 20, RXCSR2 21, RXCOUNT 22/23,
// TXTYPE 24, RXTYPE 26 (protocol in bits 5:4, 2 bulk; the target endpoint in 3:0).
#include "probe.h"

#define CON0    (*(volatile uint32_t *)0x11800u)
#define CON1    (*(volatile uint32_t *)0x11804u)
#define EP0_CNT (*(volatile uint32_t *)0x11808u)
#define EP1_CNT (*(volatile uint32_t *)0x1180Cu)
#define EP0_ADR (*(volatile uint32_t *)0x11818u)
#define EP1_TADR (*(volatile uint32_t *)0x1181Cu)
#define EP1_RADR (*(volatile uint32_t *)0x11820u)
#define IO_CON0 (*(volatile uint32_t *)0x51000u)
#define IO_CON1 (*(volatile uint32_t *)0x51004u)
enum { S_FADDR = 0, S_POWER = 1, S_INTRUSB = 6, S_INTRUSBE = 11, S_FRAME1 = 12, S_FRAME2 = 13, S_INDEX = 14, S_DEVCTL = 15,
       S_TXMAXP = 16, S_CSR0 = 17, S_TXCSR1 = 17, S_TXCSR2 = 18, S_RXMAXP = 19, S_RXCSR1 = 20, S_RXCSR2 = 21,
       S_COUNT0 = 22, S_RXCOUNT1 = 22, S_RXCOUNT2 = 23, S_TXTYPE = 24, S_RXTYPE = 26 };

static hb_probe_ctx_t* C;
static uint32_t s_timeouts;
static uint8_t s_ep0[64 + 8] __attribute__((aligned(8)));
static uint8_t s_tx[64 + 8] __attribute__((aligned(8)));
static uint8_t s_rx[576] __attribute__((aligned(8)));   // (64 would do; room if the DMA goes on past a packet)

static void wait_us(uint32_t us) { for (uint32_t t = p_ticks(); p_ticks() - t < 24u * us;) C->wdt_feed(); }
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
static uint32_t le32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// ---- EP0 (control): 0 done, < 0 stall (-1) / no answer (-2) / SIE (-3) / time out (-4); x10 - stage
static int ep0_wait(uint32_t mask, uint32_t want)
{
    for (uint32_t t = p_ticks(); p_ticks() - t < 24000u * 300u;) {
        sie_wr(S_INDEX, 0);
        const uint32_t v = sie_rd(S_CSR0);
        if (v == 0xFFFFu) return -3;
        if (v & 0x04u) { sie_wr(S_CSR0, 0); return -1; }
        if (v & 0x10u) { sie_wr(S_CSR0, 0); return -2; }
        if ((v & mask) == want) return 0;
    }
    return -4;
}
static uint32_t s_maxp0 = 8;
static int ctrl(const uint8_t setup[8], uint8_t* in, uint32_t* got)   // a control transfer, IN data or none
{
    const uint32_t want = setup[6] | (uint32_t)setup[7] << 8;
    sie_wr(S_INDEX, 0);
    for (int i = 0; i < 8; i++) s_ep0[i] = setup[i];
    __asm__ volatile("csync" ::: "memory");
    EP0_ADR = (uint32_t)(uintptr_t)s_ep0;
    EP0_CNT = 8;
    wait_us(50);                                               // the DMA into the FIFO first (without it the setup went
    sie_wr(S_CSR0, 0x0A);                                      //  unanswered; usbhost.c had a read here): SetupPkt | TxPktRdy
    int r = ep0_wait(0x02, 0x00);
    if (r) return r * 10 - 1;
    if (got) *got = 0;
    if (want && in) {
        uint32_t n = 0;
        do {
            sie_wr(S_CSR0, 0x20);                              // ReqPkt
            r = ep0_wait(0x01, 0x01);
            if (r) return r * 10 - 2;
            n = sie_rd(S_COUNT0);
            __asm__ volatile("ssync" ::: "memory");
            for (uint32_t i = 0; i < n && *got < want && *got < 255; i++) in[(*got)++] = s_ep0[i];
            sie_wr(S_CSR0, 0x00);
        } while (n == s_maxp0 && *got < want);
        sie_wr(S_CSR0, 0x42);                                  // status: an OUT with no data
        r = ep0_wait(0x02, 0x00);
    } else {
        sie_wr(S_CSR0, 0x60);                                  // status: an IN with no data (StatusPkt | ReqPkt)
        r = ep0_wait(0x01, 0x01);
    }
    sie_wr(S_CSR0, 0x00);
    return r ? r * 10 - 3 : 0;
}

// ---- EP1 (bulk): 0 done, < 0 stall (-1) / error (-2) / NAK time out or data error (-5) / time out (-4)
static int bulk_out(const uint8_t* p, uint32_t n)
{
    sie_wr(S_INDEX, 1);
    for (uint32_t i = 0; i < n; i++) s_tx[i] = p[i];
    __asm__ volatile("csync" ::: "memory");
    EP1_TADR = (uint32_t)(uintptr_t)s_tx;
    EP1_CNT = n;
    wait_us(50);                                               // (the DMA into the FIFO first)
    sie_wr(S_TXCSR1, 0x01);                                    // TxPktRdy
    for (uint32_t t = p_ticks(); p_ticks() - t < 24000u * 500u;) {
        sie_wr(S_INDEX, 1);
        const uint32_t v = sie_rd(S_TXCSR1);
        if (v == 0xFFFFu) return -3;
        if (v & 0x20u) { sie_wr(S_TXCSR1, 0); return -1; }       // RxStall
        if (v & 0x04u) { sie_wr(S_TXCSR1, 0); return -2; }       // Error
        if (v & 0x80u) { sie_wr(S_TXCSR1, 0); return -5; }       // NAK time out
        if (!(v & 0x01u)) return 0;
    }
    return -4;
}
static uint32_t s_rxLog[8 * 4], s_rxLogN;        // the first 8 packets in: their size, their first 12 bytes
static int bulk_in(uint8_t* p, uint32_t max, uint32_t* got)
{
    sie_wr(S_INDEX, 1);
    EP1_RADR = (uint32_t)(uintptr_t)s_rx;
    sie_wr(S_RXCSR1, 0x20);                                    // ReqPkt
    for (uint32_t t = p_ticks(); p_ticks() - t < 24000u * 500u;) {
        sie_wr(S_INDEX, 1);
        const uint32_t v = sie_rd(S_RXCSR1);
        if (v == 0xFFFFu) return -3;
        if (v & 0x40u) { sie_wr(S_RXCSR1, 0); return -1; }       // RxStall
        if (v & 0x04u) { sie_wr(S_RXCSR1, 0); return -2; }       // Error
        if (v & 0x08u) { sie_wr(S_RXCSR1, 0); return -5; }       // DataError / NAK time out
        if (v & 0x01u) {
            const uint32_t n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) & 7u) << 8;
            wait_us(50);                                       // (the DMA out of the FIFO first, as for TX)
            __asm__ volatile("ssync" ::: "memory");
            if (s_rxLogN < 8) { s_rxLog[4 * s_rxLogN] = n; s_rxLog[4 * s_rxLogN + 1] = le32(s_rx); s_rxLog[4 * s_rxLogN + 2] = le32(s_rx + 4);
                                s_rxLog[4 * s_rxLogN + 3] = le32(s_rx + 8); s_rxLogN++; }
            for (uint32_t i = 0; i < n && i < max; i++) p[i] = s_rx[i];
            *got = n;
            sie_wr(S_RXCSR1, 0x10);                            // FlushFIFO (RxPktRdy cleared with it): else the next
            return 0;                                          //  packet lands after this one, the count adding up
        }
    }
    return -4;
}

// ---- Bulk-Only Transport: a CBW, the data (IN), the CSW. 0 done (the CSW's status in *st), < 0 the step
static uint32_t s_tag = 1, s_inMaxp = 64, s_badCsw;
static int msc(const uint8_t* cb, uint32_t cbn, uint8_t* data, uint32_t len, uint32_t* st)
{
    uint8_t w[31] = {'U', 'S', 'B', 'C'};
    w[4] = (uint8_t)s_tag; w[5] = (uint8_t)(s_tag >> 8); s_tag++;
    w[8] = (uint8_t)len; w[9] = (uint8_t)(len >> 8); w[10] = (uint8_t)(len >> 16); w[11] = (uint8_t)(len >> 24);
    w[12] = len ? 0x80 : 0x00; w[13] = 0; w[14] = (uint8_t)cbn;
    for (uint32_t i = 0; i < cbn; i++) w[15 + i] = cb[i];
    int r = bulk_out(w, 31);
    if (r) return r * 10 - 1;
    uint32_t have = 0, n = 0;
    while (have < len) {
        r = bulk_in(data + have, len - have, &n);
        if (r) return r * 10 - 2;
        have += n;
        if (n < s_inMaxp) break;
    }
    uint8_t s[16];
    r = bulk_in(s, 13, &n);
    if (r) return r * 10 - 3;
    if (n != 13 || s[0] != 'U' || s[1] != 'S' || s[2] != 'B' || s[3] != 'S') { s_badCsw = n; return -40; }
    *st = s[12];
    return 0;
}

static int32_t run(hb_probe_ctx_t* c)
{
    C = c;
    char b[48], *p;
    const uint32_t secs = p_arg32(c, 0, 20);
    C->host->fill(0, 0, 240, 240, 0);
    line(0, "USB STICK PROBE", YELLOW);
    line(1, "Unplug the computer.", WHITE);
    line(2, "Plug in the powered OTG", WHITE);
    line(3, "adapter with the stick.", WHITE);
    for (uint32_t s = secs; s > 0; s--) {
        p = str(b, "host mode in "); p = dec(p, s); p = str(p, " s"); *p = 0;
        line(5, b, GREEN);
        wait_us(1000000);
    }

    p_irq_off();
    CON0 = 0; CON1 = 0; IO_CON0 = 0x0Cu; wait_us(20000);     // usb_sie_close
    IO_CON0 = 0; IO_CON1 = 0; wait_us(1000);                    // usb_h_sie_init
    IO_CON0 |= (1u << 9) | (1u << 10);
    IO_CON0 &= ~((1u << 6) | (1u << 7));
    IO_CON0 |= (1u << 4) | (1u << 5);                          // the host's pull-downs
    IO_CON0 &= ~((1u << 8) | (1u << 11));
    CON0 |= 0x2Du;                                             // PHY_ON | USB_NRST | TM1 | VBUS: bit 3 starts the bus (usbhost.c)
    wait_us(1000);
    sie_wr(S_INTRUSBE, 0);
    line(5, s_timeouts ? "SIE: no answer" : "host mode on", s_timeouts ? RED : GREEN);

    int r = -99;
    static uint8_t d[520];                                     // (static: the probe's RAM, not the main loop's stack)
    uint32_t dev = 0, t0 = p_ticks(), got = 0, st = 0;
    sie_wr(S_DEVCTL, 0x03);                                    // Session | HostReq
    while (p_ticks() - t0 < 24000000u * 20u) {                 // a device: FSDev / LSDev (20 s)
        dev = sie_rd(S_DEVCTL);
        if (dev & 0x60u) break;
        if (!(dev & 1u)) sie_wr(S_DEVCTL, 0x03);
        wait_us(10000);
    }
    p_hex(c, "devctl", dev);
    if (!(dev & 0x60u)) { line(6, "no device", RED); goto done; }
    if (!(dev & 0x40u)) CON0 |= 2u;                            // low speed
    sie_wr(S_FADDR, 0);
    line(6, (dev & 0x40u) ? "a full-speed device" : "a low-speed device", GREEN);

    // enumeration
    static const uint8_t kDev8[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00};
    // as the run that worked (usbhost.c): the session off and on with the device there, 200 ms, a 50 ms
    // reset, 50 ms; up to three times. The frame counter's move in 10 ms before each try is kept.
    uint32_t tries[3 * 3], nt = 0;
    for (int k = 0; k < 3; k++) {
        sie_wr(S_DEVCTL, 0x00); wait_us(100000);
        sie_wr(S_DEVCTL, 0x03); wait_us(200000);
        sie_wr(S_POWER, sie_rd(S_POWER) | 8u); wait_us(50000);
        sie_wr(S_POWER, sie_rd(S_POWER) & ~8u); wait_us(50000);
        const uint32_t g0 = sie_rd(S_FRAME1) | sie_rd(S_FRAME2) << 8;
        wait_us(10000);
        const uint32_t mv = ((sie_rd(S_FRAME1) | sie_rd(S_FRAME2) << 8) - g0) & 0x7FFu;
        sie_wr(S_FADDR, 0);
        r = ctrl(kDev8, d, &got);
        tries[3 * nt] = sie_rd(S_DEVCTL); tries[3 * nt + 1] = mv; tries[3 * nt + 2] = (uint32_t)r; nt++;
        p = str(b, "try "); p = dec(p, (uint32_t)k + 1); p = str(p, ": frames "); p = dec(p, mv);
        p = str(p, r ? " no answer" : " ok"); *p = 0;
        line(7, b, r ? YELLOW : GREEN);
        if (!r) break;
    }
    p_words(c, "tries_devctl_frames10ms_rc", tries, 3 * nt);
    p_i32(c, "get_device_8", r);
    if (r) { p = str(b, "descriptor: error "); p = dec(p, (uint32_t)-r); *p = 0; line(7, b, RED); goto done; }
    s_maxp0 = d[7] ? d[7] : 8;
    static const uint8_t kAddr[8] = {0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};   // SET_ADDRESS 1
    r = ctrl(kAddr, 0, 0);
    p_i32(c, "set_address", r);
    if (r) { line(7, "SET_ADDRESS failed", RED); goto done; }
    wait_us(5000);
    sie_wr(S_FADDR, 1);
    static const uint8_t kDev[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00};
    r = ctrl(kDev, d, &got);
    p_i32(c, "get_device", r);
    p_bytes(c, "device", d, got);
    if (r || got < 18) { line(7, "device descriptor failed", RED); goto done; }
    const uint32_t vid = d[8] | (uint32_t)d[9] << 8, pid = d[10] | (uint32_t)d[11] << 8;
    p = str(b, "VID "); p = hex(p, vid, 4); p = str(p, " PID "); p = hex(p, pid, 4); *p = 0;
    line(7, b, GREEN);
    static const uint8_t kCfg[8] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0xFF, 0x00};   // GET_DESCRIPTOR(config), 255
    r = ctrl(kCfg, d, &got);
    p_i32(c, "get_config", r);
    p_bytes(c, "config", d, got);
    if (r || got < 9) { line(8, "config descriptor failed", RED); goto done; }
    uint32_t cfg = d[5], ifc = 0xFF, epIn = 0, epOut = 0, mpIn = 64, mpOut = 64, cls = 0, sub = 0, proto = 0;
    for (uint32_t i = 0; i + 2 <= got && d[i] >= 2; i += d[i]) {
        if (d[i + 1] == 4 && i + 9 <= got) {                   // an interface
            if (ifc != 0xFF && epIn && epOut) break;           // (the first mass-storage one is enough)
            if (d[i + 5] == 8) { ifc = d[i + 2]; cls = d[i + 5]; sub = d[i + 6]; proto = d[i + 7]; epIn = epOut = 0; }
            else if (ifc == 0xFF) { cls = d[i + 5]; sub = d[i + 6]; proto = d[i + 7]; }
        } else if (d[i + 1] == 5 && i + 7 <= got && ifc != 0xFF && (d[i + 3] & 3) == 2) {   // a bulk endpoint
            const uint32_t mp = d[i + 4] | (uint32_t)d[i + 5] << 8;
            if (d[i + 2] & 0x80) { epIn = d[i + 2] & 15; mpIn = mp; } else { epOut = d[i + 2] & 15; mpOut = mp; }
        }
    }
    const uint32_t eps[8] = {cfg, ifc, cls, sub, proto, epIn, epOut, mpIn | mpOut << 16};
    p_words(c, "cfg_ifc_class_sub_proto_in_out_maxp", eps, 8);
    p = str(b, "class "); p = hex(p, cls, 2); p = str(p, "/"); p = hex(p, sub, 2); p = str(p, "/"); p = hex(p, proto, 2);
    p = str(p, " in "); p = dec(p, epIn); p = str(p, " out "); p = dec(p, epOut); *p = 0;
    line(8, b, cls == 8 && proto == 0x50 && epIn && epOut ? GREEN : RED);
    if (!(cls == 8 && proto == 0x50 && epIn && epOut)) { line(9, "not a bulk-only drive", RED); goto done; }
    uint8_t setc[8] = {0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};   // SET_CONFIGURATION
    setc[2] = (uint8_t)cfg;
    r = ctrl(setc, 0, 0);
    p_i32(c, "set_config", r);
    if (r) { line(9, "SET_CONFIGURATION failed", RED); goto done; }

    // EP1 both ways, to the stick's bulk endpoints, data toggles cleared
    s_inMaxp = mpIn;
    sie_wr(S_INDEX, 1);
    sie_wr(S_TXMAXP, 0xFF); sie_wr(S_TXTYPE, 0x20u | epOut); sie_wr(S_TXCSR2, 0); sie_wr(S_TXCSR1, 0x48);   // ClrDataTog | FlushFIFO
    sie_wr(S_RXMAXP, 0xFF); sie_wr(S_RXTYPE, 0x20u | epIn); sie_wr(S_RXCSR2, 0); sie_wr(S_RXCSR1, 0x90);
    p_hex(c, "ep1_txcsr1", sie_rd(S_TXCSR1)); p_hex(c, "ep1_rxcsr1", sie_rd(S_RXCSR1));

    // the drive
    static const uint8_t kInq[6] = {0x12, 0x00, 0x00, 0x00, 36, 0x00};
    r = msc(kInq, 6, d, 36, &st);
    p_i32(c, "inquiry", r); p_u32(c, "inquiry_status", st);
    if (r) { p = str(b, "INQUIRY: error "); p = dec(p, (uint32_t)-r); *p = 0; line(9, b, RED); goto done; }
    p_bytes(c, "inquiry_data", d, 36);
    p = b;
    for (int i = 8; i < 32 && p < b + 30; i++) *p++ = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : ' ';
    *p = 0;
    line(9, b, GREEN);
    static const uint8_t kTur[6] = {0x00, 0, 0, 0, 0, 0};
    for (int i = 0; i < 5; i++) { if (msc(kTur, 6, d, 0, &st) == 0 && st == 0) break; wait_us(100000); }   // TEST UNIT READY
    static const uint8_t kCap[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    r = msc(kCap, 10, d, 8, &st);
    p_i32(c, "read_capacity", r); p_u32(c, "read_capacity_status", st);
    if (!r) {
        const uint32_t last = be32(d), bs = be32(d + 4);
        p_u32(c, "last_block", last); p_u32(c, "block_size", bs);
        p = str(b, "size "); p = dec(p, (uint32_t)(((uint64_t)(last + 1) * bs) >> 20)); p = str(p, " MB, block "); p = dec(p, bs); *p = 0;
        line(10, b, GREEN);
    }
    static const uint8_t kRd[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0};   // READ(10): block 0, one block
    for (int i = 0; i < 512; i++) d[i] = 0;
    r = msc(kRd, 10, d, 512, &st);
    p_i32(c, "read_block0", r); p_u32(c, "read_status", st);
    if (r) { p = str(b, "READ(10): error "); p = dec(p, (uint32_t)-r); *p = 0; line(11, b, RED); goto done; }
    p_bytes(c, "block0", d, 512);
    const int sig = d[510] == 0x55 && d[511] == 0xAA;
    p = str(b, "block 0 read, "); p = str(p, sig ? "55 AA" : "no 55 AA");
    if (sig && d[0x1C2]) { p = str(p, " part "); p = hex(p, d[0x1C2], 2); p_u32(c, "part1_lba", le32(d + 0x1C6)); }
    *p = 0;
    line(11, b, sig ? GREEN : YELLOW);
    r = 0;

done:
    p_words(c, "rx_packets_n_bytes0_4_8", s_rxLog, 4 * s_rxLogN);
    p_u32(c, "bad_csw_n", s_badCsw);
    p_i32(c, "rc", r);
    p_u32(c, "sie_timeouts", s_timeouts);
    CON0 = 0; CON1 = 0; IO_CON0 = 0x0Cu;
    line(12, "Plug the computer back in. 8 s", YELLOW);
    for (;;) ;
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "USBSTICK", run};
