// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// usbmsc: see usbmsc.h. Registers (fm1-dock firmware/hal/fm1_usb.h; bits from the AC79 SDK's usb.h):
//   CON0 0x11800: 0 PHY_ON, 1 LOW_SPEED, 2 USB_NRST, 3 TM1, 4 CID (1 = device), 5 VBUS, 16 DM_SE, 17 DP_SE;
//   CON1 0x11804: the SIE mailbox; EP0_CNT 0x11808 / EP1_CNT 0x1180C start a TX DMA; EP0_ADR 0x11818,
//   EP1_TADR 0x1181C, EP1_RADR 0x11820 the DMA buffers; IO_CON0 0x51000: 4/5 the D+/D- pull-downs, 6/7 the
//   pull-ups, 9/10 their input enables. The SIE (MUSB, indexed): FADDR 0, POWER 1, INTRUSBE 11, FRAME 12/13,
//   INDEX 14, DEVCTL 15, TXMAXP 16, CSR0/TXCSR1 17, TXCSR2 18, RXMAXP 19, RXCSR1 20, RXCSR2 21,
//   COUNT0/RXCOUNT 22/23, TXTYPE 24, RXTYPE 26. EP0 is control; EP1 both ways for the stick's bulk endpoints.
#include "usbmsc.h"

#define CON0     (*(volatile uint32_t *)0x11800u)
#define CON1     (*(volatile uint32_t *)0x11804u)
#define EP0_CNT  (*(volatile uint32_t *)0x11808u)
#define EP1_CNT  (*(volatile uint32_t *)0x1180Cu)
#define EP0_ADR  (*(volatile uint32_t *)0x11818u)
#define EP1_TADR (*(volatile uint32_t *)0x1181Cu)
#define EP1_RADR (*(volatile uint32_t *)0x11820u)
#define EP2_RADR (*(volatile uint32_t *)0x11828u)
#define IO_CON0  (*(volatile uint32_t *)0x51000u)
#define IO_CON1  (*(volatile uint32_t *)0x51004u)
enum { S_FADDR = 0, S_POWER = 1, S_INTRUSB = 6, S_INTRUSBE = 11, S_FRAME1 = 12, S_FRAME2 = 13, S_INDEX = 14, S_DEVCTL = 15,
       S_TXMAXP = 16, S_CSR0 = 17, S_TXCSR1 = 17, S_TXCSR2 = 18, S_RXMAXP = 19, S_RXCSR1 = 20, S_RXCSR2 = 21,
       S_COUNT0 = 22, S_RXCOUNT1 = 22, S_RXCOUNT2 = 23, S_TXTYPE = 24, S_RXTYPE = 26, S_RXINTERVAL = 27 };

uint32_t umsc_state, umsc_blocks, umsc_vid, umsc_pid, umsc_err;
char     umsc_name[25];

static const umsc_env* E;
static uint8_t s_ep0[64 + 8] __attribute__((aligned(8)));
static uint8_t s_tx[64 + 8] __attribute__((aligned(8)));
static uint8_t s_rx[576] __attribute__((aligned(8)));       // (64 would do: room if a DMA ran on past a packet)
static uint32_t s_maxp0 = 8, s_inMaxp = 64, s_tag = 1, s_ifc, s_epIn, s_epOut;
uint32_t umsc_recoveries;
static uint32_t s_gone;                                    // (behind a hub: its port said the drive is out)

static void wait_us(uint32_t us)
{
    for (uint32_t t = E->ticks(); E->ticks() - t < 24u * us;) if (E->idle) E->idle();
}
// The SIE's registers are reached through CON1: the register written, then the answer read. With the audio
// streaming on interrupts (umsc_audio_start), the main loop's accesses hold the interrupts off around that pair:
// else the handler's own access lands between and the main loop reads its answer (seen: a "pulled out" deck).
static volatile uint32_t s_aOn, s_inIsr;
static void a_pause(void);                                 // the bus to the main loop for a transfer (the audio
static void a_resume(void);                                //  stream held), and back (no-ops when not streaming)
static uint32_t s_dAddr, s_dMaxp0;                         // the drive's address and EP0 size (umsc_read's own)
#define SIE_HOLD()  int held_ = s_aOn && !s_inIsr; if (held_) __asm__ volatile("cli" ::: "memory")
#define SIE_GO()    if (held_) __asm__ volatile("csync\n\tsti" ::: "memory")
static int sie_wr(uint32_t r, uint32_t v)
{
    SIE_HOLD();
    CON1 = ((r << 8) & 0xFFFF00u) | (v & 0xFFu);
    __asm__ volatile("csync" ::: "memory");
    uint32_t n = 20000;
    while (!(CON1 & 0x8000u) && --n) ;
    SIE_GO();
    return n ? 0 : -1;
}
static uint32_t sie_rd(uint32_t r)
{
    SIE_HOLD();
    CON1 = ((r << 8) & 0xFFBF00u) | 0x4000u;
    __asm__ volatile("csync" ::: "memory");
    uint32_t n = 20000;
    uint32_t v = 0xFFFFu;
    while (!(CON1 & 0x8000u) && --n) ;
    if (n) v = CON1 & 0xFFu;
    SIE_GO();
    return v;
}
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void umsc_host(const umsc_env* e)
{
    E = e;
    CON0 = 0; CON1 = 0; IO_CON0 = 0x0Cu; wait_us(20000);     // usb_sie_close
    IO_CON0 = 0; IO_CON1 = 0; wait_us(1000);                    // usb_h_sie_init
    IO_CON0 |= (1u << 9) | (1u << 10);
    IO_CON0 &= ~((1u << 6) | (1u << 7));
    IO_CON0 |= (1u << 4) | (1u << 5);                          // the host's pull-downs
    IO_CON0 &= ~((1u << 8) | (1u << 11));
    CON0 |= 0x2Du;                                             // PHY_ON | USB_NRST | TM1 | VBUS (bit 3: else no bus)
    wait_us(1000);
    sie_wr(S_INTRUSBE, 0);
    sie_wr(S_DEVCTL, 0x03);                                    // Session | HostReq
    umsc_state = UMSC_WAIT;
    umsc_err = 0;
    s_gone = 0;
}
int umsc_attached(void)
{
    const uint32_t d = sie_rd(S_DEVCTL);
    if (d == 0xFFFFu) return 0;
    if (!(d & 1u)) sie_wr(S_DEVCTL, 0x03);                     // (the session dropped: again)
    return (d & 0x60u) != 0;
}
// still there: D+ / D- not both low (SE0) for 100 ms. A full-speed bus idles at J (D+ high); a device pulled
// out leaves the host's pull-downs: SE0. (Plain CON0 reads: cheap enough for every main-loop pass.)
static uint32_t s_se0At;
static int gone(void);
int umsc_present(void)
{
    if (s_gone) return 0;
    if (umsc_hub) {                                            // behind a hub the root lines say nothing: its port,
        static uint32_t at;                                    //  asked every 500 ms (~3 ms a time)
        const uint32_t now = E->ticks();
        if (now - at > 24000u * 500u) { at = now; a_pause(); const int g = gone(); a_resume(); return !g; }
        return 1;
    }
    if (CON0 >> 16 & 3u) { s_se0At = 0; return 1; }
    const uint32_t now = E->ticks();
    if (!s_se0At) s_se0At = now | 1u;
    return now - s_se0At < 24000u * 100u;
}
void umsc_close(void)
{
    CON0 = 0; CON1 = 0; IO_CON0 = 0x0Cu;
    umsc_state = UMSC_OFF;
}

// ---- EP0: 0 done, < 0: -1 stall, -2 no answer, -3 SIE, -4 time out; x10 - the stage (1 setup, 2 data, 3 status)
static int ep0_wait(uint32_t mask, uint32_t want)
{
    for (uint32_t t = E->ticks(); E->ticks() - t < 24000u * 300u;) {
        if (E->idle) E->idle();
        sie_wr(S_INDEX, 0);
        const uint32_t v = sie_rd(S_CSR0);
        if (v == 0xFFFFu) return -3;
        if (v & 0x04u) { sie_wr(S_CSR0, 0); return -1; }
        if (v & 0x10u) { sie_wr(S_CSR0, 0); return -2; }
        if ((v & mask) == want) return 0;
    }
    return -4;
}
static int ctrl(const uint8_t setup[8], uint8_t* in, uint32_t* got)   // a control transfer, IN data or none
{
    const uint32_t want = setup[6] | (uint32_t)setup[7] << 8;
    sie_wr(S_INDEX, 0);
    for (int i = 0; i < 8; i++) s_ep0[i] = setup[i];
    __asm__ volatile("csync" ::: "memory");
    EP0_ADR = (uint32_t)(uintptr_t)s_ep0;
    EP0_CNT = 8;
    wait_us(50);                                               // the DMA into the FIFO first (else it goes unanswered)
    sie_wr(S_CSR0, 0x0A);                                      // SetupPkt | TxPktRdy
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
            wait_us(50);
            __asm__ volatile("ssync" ::: "memory");
            for (uint32_t i = 0; i < n && *got < want; i++) in[(*got)++] = s_ep0[i];
            sie_wr(S_CSR0, 0x00);
        } while (n == s_maxp0 && *got < want);
        sie_wr(S_CSR0, 0x42);                                  // status: an OUT with no data
        r = ep0_wait(0x02, 0x00);
    } else {
        sie_wr(S_CSR0, 0x60);                                  // status: an IN with no data
        r = ep0_wait(0x01, 0x01);
    }
    sie_wr(S_CSR0, 0x00);
    return r ? r * 10 - 3 : 0;
}

// ---- EP1 (bulk): 0 done, < 0: -1 stall, -2 error, -3 SIE, -4 time out, -5 NAK time out / data error
static int bulk_out(const uint8_t* p, uint32_t n)
{
    sie_wr(S_INDEX, 1);
    for (uint32_t i = 0; i < n; i++) s_tx[i] = p[i];
    __asm__ volatile("csync" ::: "memory");
    EP1_TADR = (uint32_t)(uintptr_t)s_tx;
    EP1_CNT = n;
    wait_us(50);
    sie_wr(S_TXCSR1, 0x01);                                    // TxPktRdy
    for (uint32_t t = E->ticks(); E->ticks() - t < 24000u * 500u;) {
        if (E->idle) E->idle();
        sie_wr(S_INDEX, 1);
        const uint32_t v = sie_rd(S_TXCSR1);
        if (v == 0xFFFFu) return -3;
        if (v & 0x20u) { sie_wr(S_TXCSR1, 0); return -1; }
        if (v & 0x04u) { sie_wr(S_TXCSR1, 0); return -2; }
        if (v & 0x80u) { sie_wr(S_TXCSR1, 0); return -5; }
        if (!(v & 0x01u)) return 0;
    }
    return -4;
}
static int bulk_in(uint8_t* p, uint32_t max, uint32_t* got)
{
    sie_wr(S_INDEX, 1);
    EP1_RADR = (uint32_t)(uintptr_t)s_rx;
    sie_wr(S_RXCSR1, 0x20);                                    // ReqPkt
    for (uint32_t t = E->ticks(); E->ticks() - t < 24000u * 500u;) {
        if (E->idle) E->idle();
        sie_wr(S_INDEX, 1);
        const uint32_t v = sie_rd(S_RXCSR1);
        if (v == 0xFFFFu) return -3;
        if (v & 0x40u) { sie_wr(S_RXCSR1, 0); return -1; }
        if (v & 0x04u) { sie_wr(S_RXCSR1, 0); return -2; }
        if (v & 0x08u) { sie_wr(S_RXCSR1, 0); return -5; }
        if (v & 0x01u) {
            const uint32_t n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) & 7u) << 8;
            wait_us(50);                                       // the DMA out of the FIFO first
            __asm__ volatile("ssync" ::: "memory");
            for (uint32_t i = 0; i < n && i < max; i++) p[i] = s_rx[i];
            *got = n;
            sie_wr(S_RXCSR1, 0x10);                            // FlushFIFO: else the next packet lands after this one
            return 0;
        }
    }
    return -4;
}

// ---- Bulk-Only Transport: the CBW, the data (IN, len bytes into data), the CSW: 0 (its status in *st), < 0
static int msc(const uint8_t* cb, uint32_t cbn, uint8_t* data, uint32_t len, uint32_t* st)
{
    uint8_t w[31] = {'U', 'S', 'B', 'C'};
    w[4] = (uint8_t)s_tag; w[5] = (uint8_t)(s_tag >> 8); s_tag++;
    w[8] = (uint8_t)len; w[9] = (uint8_t)(len >> 8); w[10] = (uint8_t)(len >> 16); w[11] = (uint8_t)(len >> 24);
    w[12] = len ? 0x80 : 0x00; w[14] = (uint8_t)cbn;
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
    if (n != 13 || s[0] != 'U' || s[1] != 'S' || s[2] != 'B' || s[3] != 'S') return -40;
    *st = s[12];
    return 0;
}

static int fail(int e) { umsc_err = (uint32_t)-e; umsc_state = UMSC_FAIL; return e; }

static uint8_t s_d[1024];                                  // (a UCA202's config descriptor: past 255 bytes)
static uint32_t s_addr;
uint32_t umsc_hub, umsc_hubid, umsc_trace[32], umsc_ntrace;
static void tr(uint32_t v) { if (umsc_ntrace < 32) umsc_trace[umsc_ntrace++] = v; }

static void set_addr(uint32_t a) { s_addr = a; sie_wr(S_FADDR, a); }

// the device just reset, at address 0: its descriptors, address addr: 0 (s_d the config descriptor, *got its
// length, *cls the device class), < 0: -100 the first 8 bytes, -200 the address, -300 device, -400 config
static int dev_enum(uint32_t addr, uint32_t* got, uint32_t* cls)
{
    static const uint8_t kDev8[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00};
    int r = -1;
    set_addr(0);
    for (int k = 0; k < 3 && r; k++) { s_maxp0 = 8; r = ctrl(kDev8, s_d, got); if (r) wait_us(10000); }
    if (r) return -100 + r;
    s_maxp0 = s_d[7] ? s_d[7] : 8;
    uint8_t sa[8] = {0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    sa[2] = (uint8_t)addr;
    if ((r = ctrl(sa, 0, 0)) != 0) return -200 + r;
    wait_us(5000);
    set_addr(addr);
    static const uint8_t kDev[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00};
    if ((r = ctrl(kDev, s_d, got)) != 0 || *got < 18) return -300 + r;
    umsc_vid = s_d[8] | (uint32_t)s_d[9] << 8; umsc_pid = s_d[10] | (uint32_t)s_d[11] << 8;
    *cls = s_d[4];
    uint8_t cfg[8] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0x09, 0x00};   // its first 9 bytes: wTotalLength,
    if ((r = ctrl(cfg, s_d, got)) != 0 || *got < 9) return -400 + r;      //  then all of it (up to 1 KiB)
    uint32_t total = s_d[2] | (uint32_t)s_d[3] << 8;
    if (total > sizeof s_d) total = sizeof s_d;
    cfg[6] = (uint8_t)total; cfg[7] = (uint8_t)(total >> 8);
    if ((r = ctrl(cfg, s_d, got)) != 0 || *got < 9) return -400 + r;
    if (s_d[9 + 5] == 9 && *got >= 18 && s_d[10] == 4) *cls = 9;    // (a hub by its interface)
    return 0;
}

// the device enumerated (s_d its config descriptor): a bulk-only drive, configured, INQUIRY, READ CAPACITY:
// 0, < 0: -500 not bulk-only, -600 set config, -700 inquiry, -800 capacity, -801 not 512-byte blocks
static int msc_open(uint32_t got)
{
    uint32_t st = 0;
    int r;
    uint8_t* d = s_d;
    uint32_t cfg = d[5], ifc = 0xFF, epIn = 0, epOut = 0, mpIn = 64, proto = 0;
    for (uint32_t i = 0; i + 2 <= got && d[i] >= 2; i += d[i]) {
        if (d[i + 1] == 4 && i + 9 <= got) {
            if (ifc != 0xFF && epIn && epOut) break;
            if (d[i + 5] == 8) { ifc = d[i + 2]; proto = d[i + 7]; epIn = epOut = 0; }
        } else if (d[i + 1] == 5 && i + 7 <= got && ifc != 0xFF && (d[i + 3] & 3) == 2) {
            if (d[i + 2] & 0x80) { epIn = d[i + 2] & 15; mpIn = d[i + 4] | (uint32_t)d[i + 5] << 8; }
            else epOut = d[i + 2] & 15;
        }
    }
    if (proto != 0x50 || !epIn || !epOut) return -500;         // not a bulk-only drive
    s_ifc = ifc; s_epIn = epIn; s_epOut = epOut;
    uint8_t setc[8] = {0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    setc[2] = (uint8_t)cfg;
    if ((r = ctrl(setc, 0, 0)) != 0) return -600 + r;
    s_inMaxp = mpIn;
    s_tag = 1;
    sie_wr(S_INDEX, 1);
    sie_wr(S_TXMAXP, 0xFF); sie_wr(S_TXTYPE, 0x20u | epOut); sie_wr(S_TXCSR2, 0); sie_wr(S_TXCSR1, 0x48);   // ClrDataTog | FlushFIFO
    sie_wr(S_RXMAXP, 0xFF); sie_wr(S_RXTYPE, 0x20u | epIn); sie_wr(S_RXCSR2, 0); sie_wr(S_RXCSR1, 0x90);
    static const uint8_t kInq[6] = {0x12, 0x00, 0x00, 0x00, 36, 0x00};
    if ((r = msc(kInq, 6, d, 36, &st)) != 0) return -700 + r;
    for (int i = 0; i < 24; i++) umsc_name[i] = (d[8 + i] >= 32 && d[8 + i] < 127) ? (char)d[8 + i] : ' ';
    umsc_name[24] = 0;
    static const uint8_t kTur[6] = {0x00, 0, 0, 0, 0, 0};
    for (int i = 0; i < 5; i++) { if (msc(kTur, 6, d, 0, &st) == 0 && st == 0) break; wait_us(100000); }
    static const uint8_t kCap[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if ((r = msc(kCap, 10, d, 8, &st)) != 0 || st) return -800 + r;
    if (be32(d + 4) != 512) return -801;                       // (512-byte blocks only: the deck's card is)
    umsc_blocks = be32(d) + 1;
    return 0;
}

// ---- USB-MIDI (audio class, MIDIStreaming: a DJ2Go): its IN endpoint on the controller's EP2 (its own DMA
// buffer and data toggles; FADDR swapped to it for each poll, as for a hub)
uint32_t umsc_midiid;
static uint32_t s_mAddr, s_mEp, s_mInt;
static uint8_t s_mrx[64 + 8] __attribute__((aligned(8)));
static int midi_open(uint32_t got)                             // the device enumerated: 0 it's MIDI (configured), < 0 not
{
    const uint8_t* d = s_d;
    uint32_t inMidi = 0, ep = 0, type = 0;
    for (uint32_t i = 0; i + 2 <= got && d[i] >= 2; i += d[i]) {
        if (d[i + 1] == 4 && i + 9 <= got) { if (ep) break; inMidi = d[i + 5] == 1 && d[i + 6] == 3; }
        else if (d[i + 1] == 5 && i + 7 <= got && inMidi && (d[i + 2] & 0x80) && (d[i + 3] & 2)) {
            ep = d[i + 2] & 15; type = d[i + 3] & 3;
        }
    }
    if (!ep) return -1;
    uint8_t setc[8] = {0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    setc[2] = d[5];
    if (ctrl(setc, 0, 0)) return -2;
    s_mAddr = s_addr; s_mEp = ep; s_mInt = type == 3;
    umsc_midiid = umsc_vid << 16 | umsc_pid;
    sie_wr(S_INDEX, 2);
    sie_wr(S_RXMAXP, 0xFF); sie_wr(S_RXTYPE, (s_mInt ? 0x30u : 0x20u) | ep);
    sie_wr(S_RXINTERVAL, s_mInt ? 1 : 2);                      // (bulk: a NAK limit of 2 frames; interrupt: every frame)
    sie_wr(S_RXCSR2, 0); sie_wr(S_RXCSR1, 0x90);               // ClrDataTog | FlushFIFO
    sie_wr(S_INDEX, 1);
    return 0;
}
int umsc_midi(uint8_t* dst)                                    // a poll: the bytes read (USB-MIDI packets), 0 none
{
    if (!s_mAddr || (umsc_state != UMSC_READY && !s_aOn)) return 0;
    a_pause();                                                 // (the audio stream held, if it's on)
    const uint32_t fa = s_addr;
    uint32_t n = 0;
    sie_wr(S_FADDR, s_mAddr);
    sie_wr(S_INDEX, 2);
    EP2_RADR = (uint32_t)(uintptr_t)s_mrx;
    sie_wr(S_RXCSR1, 0x20);                                    // ReqPkt
    for (uint32_t t = E->ticks(); E->ticks() - t < 24000u * 3u;) {
        const uint32_t v = sie_rd(S_RXCSR1);
        if (v == 0xFFFFu) break;
        if (v & 0x4Cu) { sie_wr(S_RXCSR1, 0); break; }         // stall / error / NAK limit: nothing this time
        if (v & 0x01u) {
            n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) & 7u) << 8;
            if (n > 64) n = 64;
            wait_us(50);
            __asm__ volatile("ssync" ::: "memory");
            for (uint32_t i = 0; i < n; i++) dst[i] = s_mrx[i];
            sie_wr(S_RXCSR1, 0x10);
            break;
        }
    }
    if (!n && (sie_rd(S_RXCSR1) & 0x20u)) { sie_wr(S_RXCSR1, 0); sie_wr(S_RXCSR1, 0x10); }   // (still asking: off)
    sie_wr(S_INDEX, 1);
    sie_wr(S_FADDR, fa);
    a_resume();
    return (int)n;
}

// ---- USB audio in (audio class 1, AudioStreaming: a UCA202's line in): found and configured; its alternate
// setting, isochronous IN endpoint and format kept (umsc_audio) for whoever streams it
umsc_audio_t umsc_audio;
static int audio_open(uint32_t got)
{
    const uint8_t* d = s_d;
    uint32_t ifc = 0, alt = 0, as = 0, ch = 0, bytes = 0, bits = 0, rate = 0;
    umsc_audio_t best = {0};
    for (uint32_t i = 0; i + 2 <= got && d[i] >= 2; i += d[i]) {
        if (d[i + 1] == 4 && i + 9 <= got) { as = d[i + 5] == 1 && d[i + 6] == 2; ifc = d[i + 2]; alt = d[i + 3]; ch = 0; }
        else if (d[i + 1] == 0x24 && as && d[i + 2] == 2 && i + 11 <= got) {   // FORMAT_TYPE (I)
            ch = d[i + 4]; bytes = d[i + 5]; bits = d[i + 6]; rate = 0;
            for (uint32_t k = 0; k < d[i + 7] && i + 11 + 3 * k <= d[i] + i; k++) {   // the highest rate it lists
                const uint32_t f = d[i + 8 + 3 * k] | (uint32_t)d[i + 9 + 3 * k] << 8 | (uint32_t)d[i + 10 + 3 * k] << 16;
                if (f > rate && f <= 48000) rate = f;
            }
        } else if (d[i + 1] == 5 && i + 7 <= got && as && alt && (d[i + 2] & 0x80) && (d[i + 3] & 3) == 1) {
            const uint32_t score = (ch == 2) * 4 + (bits == 16) * 2;
            const uint32_t bestScore = best.ep ? (best.ch == 2) * 4 + (best.bits == 16) * 2 : 0;
            if (!best.ep || score > bestScore) {
                best.ifc = ifc; best.alt = alt; best.ep = d[i + 2] & 15; best.maxp = d[i + 4] | (uint32_t)(d[i + 5] & 7) << 8;
                best.ch = ch; best.bytes = bytes; best.bits = bits; best.rate = rate;
            }
        }
    }
    if (!best.ep) return -1;
    uint8_t setc[8] = {0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    setc[2] = d[5];
    if (ctrl(setc, 0, 0)) return -2;
    best.addr = s_addr; best.maxp0 = s_maxp0;
    best.id = umsc_vid << 16 | umsc_pid;
    umsc_audio = best;
    return 0;
}

// ---- hubs (full speed: no split transactions; the devices behind them full speed too), tiered: a cheap
// 7-port hub is two 4-port chips, one on the other's port 1. Each device gets its own address (2, 3, ...);
// the SIE has one FADDR: a request to a hub swaps it (and EP0's packet size) in and back.
typedef struct { uint32_t addr, maxp0; } hub_t;
static hub_t s_parent;
static uint32_t s_drv, s_dvid;                                 // the drive's address, its VID << 16 | PID                                         // the drive's hub
static uint32_t s_next;                                        // the next address to give out
static int hub_req(const hub_t* hb, uint8_t rt, uint8_t rq, uint32_t val, uint32_t idx, uint8_t* in, uint32_t len)
{
    const uint8_t s[8] = {rt, rq, (uint8_t)val, (uint8_t)(val >> 8), (uint8_t)idx, (uint8_t)(idx >> 8),
                          (uint8_t)len, (uint8_t)(len >> 8)};
    const uint32_t fa = s_addr, mp = s_maxp0;
    uint32_t got = 0;
    sie_wr(S_FADDR, hb->addr); s_maxp0 = hb->maxp0;
    int r = ctrl(s, in, &got);
    sie_wr(S_FADDR, fa); s_maxp0 = mp;
    return r ? r : (len && got < len ? -9 : 0);
}
enum { P_ENABLE = 1, P_RESET = 4, P_POWER = 8, C_P_CONNECTION = 16, C_P_RESET = 20 };
static int port_status(const hub_t* hb, uint32_t p, uint32_t* st)   // wPortStatus | wPortChange << 16
{
    uint8_t b[4];
    int r = hub_req(hb, 0xA3, 0x00, 0, p, b, 4);
    *st = r ? 0 : b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return r;
}

// a hub just enumerated at s_addr (s_d its config descriptor): its ports powered, each with something plugged in
// reset and tried in turn: a drive (done), another hub (its ports searched too), anything else (its port
// switched off: a DJ2Go there doesn't get in the way): 0, < 0: -910 set config, -920 hub descriptor, -930
// nothing plugged in, -940 a port's reset, -950 a low-speed device (no PRE packets here), or the drive's error
static int hub_open(uint32_t depth)
{
    const hub_t hb = {s_addr, s_maxp0};
    if (!umsc_hubid) umsc_hubid = umsc_vid << 16 | umsc_pid;
    uint8_t setc[8] = {0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    setc[2] = s_d[5];
    int r = ctrl(setc, 0, 0);
    if (r) return -910 + r;
    uint8_t h[9];
    if ((r = hub_req(&hb, 0xA0, 0x06, 0x2900, 0, h, 9)) != 0) return -920 + r;
    const uint32_t n = h[2] > 15 ? 15 : h[2], good = h[5] * 2u;
    tr(0xA0000000u | hb.addr << 24 | n << 16 | good);
    for (uint32_t p = 1; p <= n; p++) hub_req(&hb, 0x23, 0x03, P_POWER, p, 0, 0);
    wait_us((good + 100u) * 1000u);
    uint32_t seen = 0, st = 0;
    for (uint32_t t = E->ticks(), first = 0; E->ticks() - t < 24000u * 2500u;) {   // the devices connecting
        for (uint32_t p = 1; p <= n; p++) if (!port_status(&hb, p, &st) && (st & 1u)) seen |= 1u << p;
        if (seen && !first) first = E->ticks() | 1u;
        if (first && E->ticks() - first > 24000u * 300u) break;    // (a moment more for a slower one)
        wait_us(20000);
    }
    tr(0xB0000000u | hb.addr << 24 | seen);
    if (!seen) return -930;
    int last = -930;
    for (uint32_t p = 1; p <= n; p++) {
        if (!(seen & 1u << p)) continue;
        hub_req(&hb, 0x23, 0x01, C_P_CONNECTION, p, 0, 0);
        hub_req(&hb, 0x23, 0x03, P_RESET, p, 0, 0);
        uint32_t ok = 0;
        for (uint32_t t = E->ticks(); E->ticks() - t < 24000u * 500u;) {
        if (E->idle) E->idle();
            wait_us(10000);
            if (!port_status(&hb, p, &st) && (st & 1u << 20) && !(st & 1u << 4)) { ok = (st & 2u) != 0; break; }
        }
        hub_req(&hb, 0x23, 0x01, C_P_RESET, p, 0, 0);
        tr(0xC0000000u | hb.addr << 24 | p << 20 | (st & 0xFFFFu));
        if (!ok) { last = -940; continue; }
        wait_us(20000);                                        // reset recovery
        if (st & 1u << 9) { last = -950; hub_req(&hb, 0x23, 0x01, P_ENABLE, p, 0, 0); continue; }
        uint32_t got = 0, cls = 0;
        const uint32_t a = s_next++;
        r = dev_enum(a, &got, &cls);
        tr(0xD0000000u | a << 24 | (uint32_t)(-r & 0xFFF) << 12 | cls);
        if (!r && cls == 9 && depth < 4) { r = hub_open(depth + 1); if (!r || s_mAddr) continue; }
        else if (!r && !s_drv && msc_open(got) == 0) {
            umsc_hub = p; s_parent = hb; s_drv = a; s_dvid = umsc_vid << 16 | umsc_pid; s_dMaxp0 = s_maxp0;
            continue;
        } else if (!r && !s_mAddr && midi_open(got) == 0) { tr(0xF0000000u | a << 24 | s_mEp << 8 | s_mInt); continue; }
        else if (!r && !umsc_audio.ep && audio_open(got) == 0) { tr(0xF1000000u | a << 24 | umsc_audio.ep << 8 | umsc_audio.alt); continue; }
        else if (!r) { tr(0xF2000000u | a << 24 | got); r = -500; }   // (unknown: its descriptor's length)
        tr(0xE0000000u | a << 24 | (uint32_t)(-r & 0xFFF));
        last = r;
        set_addr(0);
        hub_req(&hb, 0x23, 0x01, P_ENABLE, p, 0, 0);           // (off again)
    }
    return s_drv ? 0 : last;
}

int umsc_open(void)
{
    uint32_t got = 0, cls = 0;
    int r = -1;
    umsc_hub = umsc_hubid = umsc_ntrace = umsc_midiid = 0;
    s_next = 2; s_drv = 0; s_mAddr = 0;
    umsc_audio.ep = 0;
    const uint32_t dev = sie_rd(S_DEVCTL);
    if (!(dev & 0x40u)) CON0 |= 2u;                            // low speed
    for (int k = 0; k < 3 && r; k++) {                         // the session again with the device there, a 50 ms reset
        sie_wr(S_DEVCTL, 0x00); wait_us(100000);
        sie_wr(S_DEVCTL, 0x03); wait_us(200000);
        sie_wr(S_POWER, sie_rd(S_POWER) | 8u); wait_us(50000);
        sie_wr(S_POWER, sie_rd(S_POWER) & ~8u); wait_us(50000);
        r = dev_enum(1, &got, &cls);
        if (r > -200) continue;                                // (only the first 8 bytes are worth a new reset)
        break;
    }
    if (!r && cls == 9) r = hub_open(1);
    else if (!r && msc_open(got) != 0) {                     // on the port itself, not a drive: MIDI or audio?
        if (midi_open(got) == 0) tr(0xF0000000u | 1u << 24 | s_mEp << 8 | s_mInt);
        else if (audio_open(got) == 0) tr(0xF1000000u | 1u << 24 | umsc_audio.ep << 8 | umsc_audio.alt);
        else return fail(-500);
        return 1;                                              // (found, but no drive: umsc_midi / umsc_audio say what)
    }
    if (r) return fail(r);
    if (s_drv) {                                               // (behind hubs: the drive's address, its IDs, back)
        set_addr(s_drv);
        umsc_vid = s_dvid >> 16; umsc_pid = s_dvid & 0xFFFFu;
    }
    s_dAddr = s_addr; if (!s_drv) s_dMaxp0 = s_maxp0;
    umsc_state = UMSC_READY;
    return 0;
}

// behind a hub the root lines stay up with the drive pulled out: a failed transfer asks the hub about its port
static int gone(void)
{
    if (s_gone) return 1;
    if (!umsc_hub) return !umsc_present();
    uint32_t st = 0;
    if (port_status(&s_parent, umsc_hub, &st) != 0 || (st & 3u) != 3u) s_gone = 1;   // (out, or the port switched
                                                                                  //  off by a bump; no answer: the hub's gone)
    return (int)s_gone;
}

// a transfer gone wrong leaves the stick (and the toggles) out of step, and every read after it fails: the
// Bulk-Only reset recovery (BOT 5.3.4): Bulk-Only Mass Storage Reset, CLEAR_FEATURE(ENDPOINT_HALT) on both
// endpoints, the host's data toggles cleared, the FIFOs flushed
static void recover(void)
{
    umsc_recoveries++;
    uint8_t rs[8] = {0x21, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    rs[4] = (uint8_t)s_ifc;
    ctrl(rs, 0, 0);
    uint8_t ch[8] = {0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ch[4] = (uint8_t)(0x80u | s_epIn); ctrl(ch, 0, 0);
    ch[4] = (uint8_t)s_epOut; ctrl(ch, 0, 0);
    sie_wr(S_INDEX, 1);
    sie_wr(S_TXCSR1, 0x48);                                    // ClrDataTog | FlushFIFO
    sie_wr(S_RXCSR1, 0x90);
}

int umsc_read(uint32_t lba, uint32_t n, uint8_t* dst)
{
    if (umsc_state != UMSC_READY || !n || n > 64) return -1;
    uint8_t cb[10] = {0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba, 0,
                      (uint8_t)(n >> 8), (uint8_t)n, 0};
    int r = 0;
    a_pause();
    if (s_aOn) { set_addr(s_dAddr); s_maxp0 = s_dMaxp0; }
    for (int k = 0; k < 3; k++) {
        if (!umsc_present()) { umsc_err = 99; r = -99; break; }   // (gone: no retries, no waiting on time-outs)
        uint32_t st = 0;
        r = msc(cb, 10, dst, n * 512u, &st);
        if (!r && !st) break;
        umsc_err = r ? (uint32_t)-r : 9u;
        if (gone()) { umsc_err = 99; r = -99; break; }
        recover();
        r = r ? r : -9;
    }
    a_resume();
    return r;
}

#ifdef UMSC_NO_AUDIO
static void a_pause(void) {}
static void a_resume(void) {}
#else                                                      // (probes/usbirq.c has its own, instrumented)
// ---- isochronous audio in, on interrupts (probes/usbirq.c; measured on a UCA202, 2026-11-26: every packet, 0
// errors, the handler <= 9 us). A request goes out at the next frame's start, so the next one is asked for when a
// packet arrives (IRQ 9, the SIE's: SIEIE, INTRRX1E EP3); the SOF interrupt (IRQ 8) asks again after 3 empty frames.
// The DMA may still be writing when the packet's interrupt comes: two buffers, the next request into the other,
// the packet taken the previous frame's. The device's address is the controller's only one (FADDR): nothing else
// may use the bus while it streams.
#define EP3_RADR    (*(volatile uint32_t *)0x11830u)
#define VEC         ((volatile uint32_t *)0x01C7FE00u)    // fm1-dock's vectors (hal/fm1_irq.h)
#define ICFG(n)     (*(volatile uint32_t *)(0x1EEF100u + 4u * ((n) >> 3)))
#define SOFIE       (1u << 10)
#define SIEIE       (1u << 11)
#define CLR_SOF_PND (1u << 12)
// fm1-dock write-protects the top of RAM (boot info, mailbox, the vectors; hal/fm1_guard.h): opened for a vector
#define DBG_WR_EN   (*(volatile uint32_t *)0x1EEE240u)
#define DBG_MSG_CLR (*(volatile uint32_t *)0x1EEE248u)
#define C0_WRLIM_EN (*(volatile uint32_t *)0x1EEE348u)
static void vec_set(uint32_t n, uint32_t v)                // (interrupts off)
{
    if (!(DBG_WR_EN & 1u)) DBG_WR_EN = 0xE7u;
    const uint32_t en = C0_WRLIM_EN;
    C0_WRLIM_EN = en & ~4u;
    VEC[n] = v;
    __asm__ volatile("csync" ::: "memory");
    DBG_MSG_CLR = 0xFFFFFFFFu;
    C0_WRLIM_EN = en;
    if (DBG_WR_EN & 1u) DBG_WR_EN = 0xE7u;
}
enum { ARING = 4096 };                                     // stereo frames (85 ms at 48 kHz), a power of two
static int16_t s_aring[2 * ARING];
static volatile uint32_t s_awr, s_ard, s_asince;
static uint8_t s_abuf0[1024 + 8] __attribute__((aligned(8))), s_abuf1[1024 + 8] __attribute__((aligned(8)));
static uint8_t* const s_abuf[2] = {s_abuf0, s_abuf1};
static uint32_t s_acur, s_aprevN, s_ach = 2, s_asb = 2, s_aOldVec8, s_aOldVec9;
static volatile uint32_t s_aPause, s_aIdle, s_aPend, s_aDepth;   // held; the request out done; its packet's length
volatile uint32_t umsc_aheld, umsc_aheldMax;               // holds, the longest (ticks)
static uint32_t s_aHeldAt;
volatile uint32_t umsc_apk, umsc_amiss, umsc_aerr, umsc_aover;

static void a_take(const uint8_t* q0, uint32_t n)          // a packet, its DMA long done: into the ring
{
    const uint32_t fb = s_asb * s_ach;
    for (uint32_t i = 0; i + fb <= n; i += fb) {
        if (s_awr - s_ard >= ARING) { umsc_aover++; break; }
        const uint8_t* q = q0 + i;
        const uint32_t k = s_awr & (ARING - 1u);
        s_aring[2 * k] = (int16_t)(q[s_asb - 2] | q[s_asb - 1] << 8);   // (the top 16 bits of each sample)
        s_aring[2 * k + 1] = (int16_t)(q[2 * s_asb - 2] | q[2 * s_asb - 1] << 8);
        s_awr++;
    }
}
static void a_packet(void)
{
    sie_wr(S_INDEX, 3);
    const uint32_t v = sie_rd(S_RXCSR1);
    uint32_t n = 0;
    if (v & 0x01u) {
        n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) & 7u) << 8;
        if (n > 1024u) n = 1024u;
        if (v & 0x0Cu) umsc_aerr++;
        umsc_apk++;
        sie_wr(S_RXCSR1, 0x10);                            // FlushFIFO
    } else if (v & 0x4Cu) { umsc_aerr++; sie_wr(S_RXCSR1, 0); }
    const uint32_t done = s_acur;
    if (s_aPause) {                                        // held: no new request; the one before taken, this one
        if (s_aprevN) a_take(s_abuf[done ^ 1u], s_aprevN); //  (its DMA maybe still going) at the resume
        s_aprevN = 0; s_aPend = n; s_aIdle = 1;
        sie_wr(S_INDEX, 1);
        return;
    }
    s_acur ^= 1u;
    EP3_RADR = (uint32_t)(uintptr_t)s_abuf[s_acur];
    sie_wr(S_RXCSR1, 0x20);                                // ReqPkt: into the other buffer, at the next frame
    sie_wr(S_INDEX, 1);
    __asm__ volatile("ssync" ::: "memory");
    if (s_aprevN) a_take(s_abuf[done ^ 1u], s_aprevN);    // the packet before (in the other buffer)
    s_aprevN = n;
    s_asince = 0;
}
void umsc_sie_irq(void)
{
    s_inIsr = 1;
    const uint32_t rx = sie_rd(4);                         // INTRRX1 (read: cleared)
    (void)sie_rd(S_INTRUSB); (void)sie_rd(2);
    if (rx & 0x08u) a_packet();
    s_inIsr = 0;
}
void umsc_sof_irq(void)
{
    s_inIsr = 1;
    CON0 |= CLR_SOF_PND;
    if (!s_aPause && ++s_asince > 3u) { umsc_amiss++; a_packet(); }   // no packet for 3 frames: ask again
    s_inIsr = 0;
}
// the entries: as fm1-dock's FM1_ISR (hal/fm1_isr.S): what a C call may clobber saved, csync before rti
__asm__(".section .text.umsc_isr_sof,\"ax\",@progbits\n"
        "\t.globl umsc_isr_sof\n"
        "umsc_isr_sof:\n"
        "\t[--sp] = {psr, rets, reti}\n"
        "\t[--sp] = {r3-r0}\n"
        "\tcall umsc_sof_irq\n"
        "\t{r3-r0} = [sp++]\n"
        "\t{psr, rets, reti} = [sp++]\n"
        "\tcsync\n"
        "\trti\n"
        ".section .text.umsc_isr_sie,\"ax\",@progbits\n"
        "\t.globl umsc_isr_sie\n"
        "umsc_isr_sie:\n"
        "\t[--sp] = {psr, rets, reti}\n"
        "\t[--sp] = {r3-r0}\n"
        "\tcall umsc_sie_irq\n"
        "\t{r3-r0} = [sp++]\n"
        "\t{psr, rets, reti} = [sp++]\n"
        "\tcsync\n"
        "\trti\n");
extern void umsc_isr_sof(void);
extern void umsc_isr_sie(void);

int umsc_audio_start(void)
{
    if (!umsc_audio.ep || s_aOn) return -1;
    s_ach = umsc_audio.ch ? umsc_audio.ch : 2; s_asb = umsc_audio.bytes >= 2 ? umsc_audio.bytes : 2;
    set_addr(umsc_audio.addr);
    s_maxp0 = umsc_audio.maxp0;
    uint8_t si[8] = {0x01, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};   // SET_INTERFACE: streaming on
    si[2] = (uint8_t)umsc_audio.alt; si[4] = (uint8_t)umsc_audio.ifc;
    const int r = ctrl(si, 0, 0);
    if (r) return -10 + r;
    s_awr = s_ard = s_asince = 0; s_acur = 0; s_aprevN = 0;
    umsc_apk = umsc_amiss = umsc_aerr = umsc_aover = 0;
    __asm__ volatile("cli" ::: "memory");
    sie_wr(S_INDEX, 3);
    sie_wr(S_RXMAXP, umsc_audio.maxp > 255 ? 0xFF : umsc_audio.maxp);
    sie_wr(S_RXTYPE, 0x10u | umsc_audio.ep);
    sie_wr(S_RXINTERVAL, 1);
    sie_wr(S_RXCSR2, 0); sie_wr(S_RXCSR1, 0x90);
    EP3_RADR = (uint32_t)(uintptr_t)s_abuf[0];
    sie_wr(S_RXCSR1, 0x20);
    sie_wr(S_INDEX, 1);
    sie_wr(7, 0); sie_wr(8, 0); sie_wr(10, 0);             // the other endpoints' interrupts off
    (void)sie_rd(2); (void)sie_rd(3); (void)sie_rd(4); (void)sie_rd(5); (void)sie_rd(S_INTRUSB);
    sie_wr(S_INTRUSBE, 0);
    sie_wr(9, 0x08);                                       // INTRRX1E: EP3
    CON0 |= CLR_SOF_PND;
    CON0 |= SOFIE | SIEIE;
    s_aOldVec8 = VEC[8]; s_aOldVec9 = VEC[9];
    vec_set(9, (uint32_t)(uintptr_t)umsc_isr_sie);
    ICFG(9) = (ICFG(9) & ~(0xFu << 4)) | (((5u << 1) | 1u) << 4);   // priority 5 (audio 3, TIMER5 4)
    vec_set(8, (uint32_t)(uintptr_t)umsc_isr_sof);
    ICFG(8) = (ICFG(8) & ~0xFu) | ((5u << 1) | 1u);        // (8 and 9 share the word: fresh each)
    s_aOn = 1;
    __asm__ volatile("csync\n\tsti" ::: "memory");
    return 0;
}
void umsc_audio_stop(void)
{
    if (!s_aOn) return;
    __asm__ volatile("cli" ::: "memory");
    CON0 &= ~(SOFIE | SIEIE);
    sie_wr(9, 0);
    ICFG(9) &= ~(0xFu << 4); ICFG(8) &= ~0xFu;
    vec_set(9, s_aOldVec9); vec_set(8, s_aOldVec8);
    sie_wr(S_INDEX, 3); sie_wr(S_RXCSR1, 0x10); sie_wr(S_INDEX, 1);
    s_aOn = 0;
    __asm__ volatile("csync\n\tsti" ::: "memory");
}
uint32_t umsc_audio_read(int16_t* lr, uint32_t frames)
{
    uint32_t n = s_awr - s_ard;
    if (n > frames) n = frames;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t k = (s_ard + i) & (ARING - 1u);
        lr[2 * i] = s_aring[2 * k]; lr[2 * i + 1] = s_aring[2 * k + 1];
    }
    s_ard += n;
    return n;
}
uint32_t umsc_audio_avail(void) { return s_awr - s_ard; }
// A transfer to another device (the stick behind the hub) needs FADDR: the stream is held first - no new request,
// the one out left to finish (its packet comes within a frame) - and asked again after, from a fresh buffer. The
// audio lost meanwhile is a gap in the ring (~5 ms for a 4 KB read: the vinyl rules ride gaps under 40 ms).
static void a_pause(void)
{
    if (!s_aOn || s_aDepth++) return;
    s_aIdle = 0; s_aPend = 0; s_aPause = 1;
    s_aHeldAt = E->ticks();
    for (uint32_t t = E->ticks(); !s_aIdle && E->ticks() - t < 24000u * 4u;) ;
    if (!s_aIdle) { sie_wr(S_INDEX, 3); sie_wr(S_RXCSR1, 0x10); sie_wr(S_INDEX, 1); s_aIdle = 1; }   // (none came)
}
static void a_resume(void)
{
    if (!s_aOn || !s_aDepth || --s_aDepth) return;
    if (s_aPend) { a_take(s_abuf[s_acur], s_aPend); s_aPend = 0; }   // the last packet: its DMA long done
    set_addr(umsc_audio.addr);
    s_maxp0 = umsc_audio.maxp0;
    s_aprevN = 0; s_asince = 0;
    s_acur ^= 1u;
    sie_wr(S_INDEX, 3);
    EP3_RADR = (uint32_t)(uintptr_t)s_abuf[s_acur];
    sie_wr(S_RXCSR1, 0x20);
    sie_wr(S_INDEX, 1);
    const uint32_t held = E->ticks() - s_aHeldAt;
    umsc_aheld++; if (held > umsc_aheldMax) umsc_aheldMax = held;
    s_aPause = 0;
}
// the interface still on the port, while it streams: its packets still coming (one a millisecond: none for 200 ms,
// it's gone). Not the root's lines: with the stream's interrupts on, CON0's line bits read SE0 with it there
// (seen on the FM-1, 2026-11-26), and asking a hub would share the bus.
uint32_t umsc_audio_con0;                                  // CON0 when it was called gone (to learn those bits)
int umsc_audio_present(void)
{
    static uint32_t pk, at;
    const uint32_t now = E->ticks();
    if (umsc_apk != pk || !at) { pk = umsc_apk; at = now | 1u; return 1; }
    if (now - at < 24000u * 200u) return 1;
    umsc_audio_con0 = CON0;
    at = 0;
    return 0;
}
#endif
