// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// hal.h as an fm1-dock module (github.com/joakimeriksson/fm1-dock, firmware/dock/hb_module.h):
// the deck runs on an M-VAVE FM-1, or in fm1-emu. fm1-dock keeps USB, the installer and the
// rescue; this file turns its events, its audio callback and its screen into what fw/ expects.
//
//   SELECT    the jog (FX + SELECT opens the track list; in   PLAY  PLAY
//             it SELECT moves, its push loads, FX tapped closes)
//   KNOB1     the tempo (the pot: 512 is 0 %)                SEQ   CUE (left of PLAY, as on a CDJ)
//   FX        SHIFT                                          EDIT  the encoder's push: keylock,
//   MIDI IN   the DJ2Go map, as over the CH32's bridge             load in the list
//
// The screen: the SSD1306's bytes (fw/ui.c, unchanged) land in a 128 x 64 picture here; each
// page that changed is scaled to 240 x 15 and put on the panel under the host's line.
// The audio: nano-lite at 22050 Hz into a ring, the render callback doubles it to 44.1 kHz.
// The card: the simulator's emulated SD card (sim/emu/sd_emu.c), serving a card image kept in
// the module's flash (tools/memcard.py), as the PY32's MEMCARD build does.
#include "hb_module.h"
#include "app.h"
#include "hal.h"
#include "ui.h"
#include "sd_emu.h"

static const hb_host_t* H;

// ---- time, inputs
uint32_t hal_ms(void) { return H->ms(); }

static uint8_t  s_pins;                           // PIN_* held
static int16_t  s_jog;                            // SELECT clicks not yet taken
static int16_t  s_pot = 512;                      // KNOB1: the tempo
uint8_t  hal_pins(void) { return s_pins; }
uint16_t hal_pot(void) { return (uint16_t)s_pot; }
int16_t  hal_jog(void) { const int16_t j = s_jog; s_jog = 0; return (int16_t)(j * 4); }   // a click is an EC11 detent

static uint8_t btnPin(uint32_t id)
{
    switch (id) {
    case HB_BTN_PLAY: return PIN_PLAY;
    case HB_BTN_SEQ:  return PIN_CUE;
    case HB_BTN_FX:   return PIN_SHIFT;
    case HB_BTN_EDIT: return PIN_PUSH;
    case HB_BTN_SEL:  return PIN_PUSH;            // SELECT's own push: loads in the list, as EDIT
    }
    return 0;
}

static void ui(uint32_t ev, int32_t arg)
{
    switch (HB_EV_KIND(ev)) {
    case HB_EV_BTN_DOWN: s_pins |= btnPin(HB_EV_ID(ev)); break;
    case HB_EV_BTN_UP:   s_pins &= (uint8_t)~btnPin(HB_EV_ID(ev)); break;
    case HB_EV_ENC:
        if (HB_EV_ID(ev) == HB_ENC_SELECT) s_jog = (int16_t)(s_jog + arg);
        else if (HB_EV_ID(ev) == HB_ENC_KNOB1) {
            int32_t p = s_pot + arg * 4;          // 4 / 1023 a click: ~0.25 % at the 16 % range
            s_pot = (int16_t)(p < 0 ? 0 : p > 1023 ? 1023 : p);
        }
        break;
    }
}

// ---- MIDI IN as the bridge's bytes: 0x01 + a USB-MIDI packet
static uint8_t s_rx[64];
static uint8_t s_rxHead, s_rxTail;
static void rxPut(uint8_t b)
{
    const uint8_t n = (uint8_t)((s_rxHead + 1) & 63);
    if (n != s_rxTail) { s_rx[s_rxHead] = b; s_rxHead = n; }
}
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (st >= 0xF0) return;
    if (st == 0xBF && d1 == 112) return;          // the USB host's filler between frames (tools/fm1host.py)
    rxPut(0x01); rxPut((uint8_t)(st >> 4)); rxPut((uint8_t)st); rxPut((uint8_t)d1); rxPut((uint8_t)d2);
}
int hal_uart_rx(void)
{
    if (s_rxTail == s_rxHead) return -1;
    const uint8_t b = s_rx[s_rxTail];
    s_rxTail = (uint8_t)((s_rxTail + 1) & 63);
    return b;
}

static sd_emu s_card;
#if FM1_HOST
// ---- the card, from the computer over USB (tools/fm1host.py)
// The deck's SD code waits for a block in the main loop, and the main loop is also where the
// blocks arrive (data()), so a block that isn't here can't be waited for. Instead:
//   - before the deck starts, the computer sends the card's blocks that aren't track data (the
//     FAT, the directory, TRACKS.BIN's header and entries): kept for good (s_pin);
//   - the track data (the mu-law and the peaks) is asked for ahead of where the deck reads,
//     into a cache; a block not there yet reads as silence (0xFF in mu-law; 0 for the peaks).
// To the computer: MIDI CCs on channel 16: 102..105 a block number, 7 bits each from the low
// end, then 106 = how many from it for the run (replaces it, unless it carries it on), or 108 =
// how many as a one-off (sent first, the run then carries on); 107 = hello (the module started).
// From it, MODULE frames (hb_module.h, data()): 'H' u32 block, u8 half, 256 bytes: half a block (the
// stream: each frame addressed, so one lost costs only itself); 512 bytes = the next block; 'L' u32 the next
// block's number (cache), 'P' u32 the same into the pinned set, 'A' u8 n + n x (u32 first,
// u32 end): the mu-law's block ranges, 'G': all pinned blocks sent, the deck can start.
#define CACHE   256u                              // 128 KB, by block number mod CACHE
#define PIN     48u
#define RANGES  64u
#define AHEAD   120u                              // blocks asked for ahead of the deck: ~2.8 s of audio (a request's
                                                  // count is one MIDI data byte: at most 127)
static uint8_t  s_cache[CACHE][512];
static uint32_t s_tag[CACHE];                     // block + 1, 0: empty
static uint32_t s_halfTag[CACHE];                 // block + 1 whose halves are coming in ('H' frames)
static uint8_t  s_halves[CACHE];                  // bit 0 / 1: that half is in
static uint8_t  s_pin[PIN][512];
static uint32_t s_pinTag[PIN];
static uint8_t  s_pins_n;
static uint32_t s_range[RANGES][2];
static uint8_t  s_ranges;
static uint32_t s_next;                           // where the next 512 bytes go
static uint8_t  s_pinning, s_ready, s_started;
static uint32_t s_reqFrom, s_reqEnd = 0, s_miss, s_got;
static uint32_t s_blocks = 4194304u;              // 2 GB, as tools/sdimage.py makes it

static void request(uint32_t lba, uint32_t n, uint32_t cc)   // cc 106: the run (replaces it), 108: a one-off
{
    if (n > 127) n = 127;                         // (one MIDI data byte)
    for (int i = 0; i < 4; i++) H->midi_send(0xBF, 102u + (uint32_t)i, lba >> (7 * i) & 0x7F);
    H->midi_send(0xBF, cc, n);
}
static int inAudio(uint32_t lba)
{
    for (uint8_t i = 0; i < s_ranges; i++) if (lba >= s_range[i][0] && lba < s_range[i][1]) return 1;
    return 0;
}
// The mu-law is read in order: one run, kept AHEAD blocks in front of the deck, in s_cache. Anything else
// (the peaks, for the waveform) has a cache of its own, s_side (a track's peaks are ~1/256 of its audio:
// 64 blocks hold 10 minutes'), and is asked for by itself, a few blocks, without moving the run; asked
// again if it hasn't come in 300 ms (a frame lost on the way).
#define SIDE 64u
static uint8_t  s_side[SIDE][512];
static uint32_t s_sideTag[SIDE];                  // block + 1, 0: empty
static uint32_t s_askedLba[4], s_askedAt[4];
static uint8_t  s_askedN;
static void aheadSide(uint32_t lba)
{
    for (uint8_t i = 0; i < 4; i++)
        if (s_askedLba[i] == lba + 1 && H->ms() - s_askedAt[i] < 300) return;   // asked for, on its way
    const uint8_t k = s_askedN++ & 3;
    s_askedLba[k] = lba + 1; s_askedAt[k] = H->ms();
    request(lba, 4, 108);
}
static uint32_t s_jumpLba, s_jumpAt;
static uint32_t s_front;                          // block + 1: the furthest the run has come in
static void jump(uint32_t lba)                    // the run from here (not again while one that covers it is < 0.5 s old)
{
    if (H->ms() - s_jumpAt < 500 && lba >= s_jumpLba && lba < s_jumpLba + AHEAD) return;
    s_jumpLba = lba; s_jumpAt = H->ms();
    s_reqFrom = lba; s_reqEnd = lba + AHEAD; s_front = lba;
    request(lba, AHEAD, 106);
}
static void ahead(uint32_t lba, int miss)
{
    if (miss || lba < s_reqFrom || lba > s_reqEnd) { jump(lba); return; }                // a jump, or not here when wanted
    s_reqFrom = lba;                                                                     // the window follows the deck
    if (s_reqEnd - lba < AHEAD / 2) { request(s_reqEnd, AHEAD / 2, 106); s_reqEnd += AHEAD / 2; }   // keep ahead
}
// A paused deck reads nothing: after a load, a cue or CUE's jump back, ask for where it stands, so the
// blocks are here when PLAY comes (else its first reads miss and the start plays silence).
static void standing(void)
{
    if (g_deck.playing || g_app.track == APP_NONE || g_app.track >= s_ranges) return;
    const uint32_t lba = s_range[g_app.track][0] + (uint32_t)lite_pos(&g_deck) / 512;
    if (s_tag[lba % CACHE] != lba + 1) jump(lba);
}
// The computer sends the run in order and resends nothing by itself: a block that didn't arrive (a
// frame lost on the way) is a gap behind the furthest one that did. Every 20 ms, the first such gap
// between the deck and there is asked for again (each block at most every 300 ms).
static uint32_t s_holeAt, s_holeLba[4], s_holeAsked[4];
static uint8_t  s_holeN;
static void holes(void)
{
    if (H->ms() - s_holeAt < 20 || g_app.track == APP_NONE || g_app.track >= s_ranges || s_front < 3) return;
    s_holeAt = H->ms();
    const uint32_t from = s_range[g_app.track][0] + (uint32_t)lite_pos(&g_deck) / 512;
    for (uint32_t b = from; b + 2 < s_front && b < from + CACHE - 8; b++) {
        if (s_tag[b % CACHE] == b + 1) continue;
        for (uint8_t i = 0; i < 4; i++) if (s_holeLba[i] == b + 1 && H->ms() - s_holeAsked[i] < 300) return;
        const uint8_t k = s_holeN++ & 3;
        s_holeLba[k] = b + 1; s_holeAsked[k] = H->ms();
        request(b, 1, 108);
        return;
    }
}
static int memRead(void* ctx, uint32_t lba, uint8_t* dst)
{
    (void)ctx;
    for (uint8_t i = 0; i < s_pins_n; i++)
        if (s_pinTag[i] == lba) { for (int k = 0; k < 512; k++) dst[k] = s_pin[i][k]; return 0; }
    if (!inAudio(lba)) {                          // the peaks
        const uint32_t c = lba % SIDE;
        if (s_sideTag[c] == lba + 1) { for (int k = 0; k < 512; k++) dst[k] = s_side[c][k]; return 0; }
        for (int k = 0; k < 512; k++) dst[k] = 0;
        aheadSide(lba);
        return 0;
    }
    const uint32_t c = lba % CACHE;
    if (s_tag[c] == lba + 1) for (int k = 0; k < 512; k++) dst[k] = s_cache[c][k];
    else {
        for (int k = 0; k < 512; k++) dst[k] = 0xFF;              // silence in mu-law
        s_miss++;
        ahead(lba, 1);
        return 0;
    }
    ahead(lba, 0);
    return 0;
}
static int data(const uint8_t* p, uint32_t n)
{
    if (n == 512) {
        if (s_pinning) {
            if (s_pins_n >= PIN) return 2;
            s_pinTag[s_pins_n] = s_next;
            for (int k = 0; k < 512; k++) s_pin[s_pins_n][k] = p[k];
            s_pins_n++;
        } else {
            const uint32_t c = s_next % CACHE;
            s_tag[c] = 0; s_halfTag[c] = 0;
            for (int k = 0; k < 512; k++) s_cache[c][k] = p[k];
            s_tag[c] = s_next + 1;
            s_got++;
        }
        s_next++;
        return 0;
    }
    const uint32_t v = n >= 5 ? (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24 : 0;
    switch (n ? p[0] : 0) {
    case 'H': {                                   // 'H' u32 block, u8 half, 256 bytes: half a block, addressed
        if (n != 262) return 3;
        const uint32_t h = p[5] & 1;
        if (!inAudio(v)) {                        // the peaks: their own cache (halves in order, the second completes it)
            const uint32_t c = v % SIDE;
            if (!h) s_sideTag[c] = 0;
            for (int k = 0; k < 256; k++) s_side[c][h * 256 + k] = p[6 + k];
            if (h) s_sideTag[c] = v + 1;
            return 0;
        }
        if (v + 1 > s_front && v < s_reqEnd) s_front = v + 1;   // the furthest block the run has reached
        const uint32_t c = v % CACHE;
        if (s_halfTag[c] != v + 1) { s_halfTag[c] = v + 1; s_halves[c] = 0; s_tag[c] = 0; }
        for (int k = 0; k < 256; k++) s_cache[c][h * 256 + k] = p[6 + k];
        s_halves[c] |= (uint8_t)(1u << h);
        if (s_halves[c] == 3) { s_tag[c] = v + 1; s_got++; }
        return 0;
    }
    case 'L': s_next = v; s_pinning = 0; return 0;
    case 'P': s_next = v; s_pinning = 1; return 0;
    case 'A':
        s_ranges = 0;
        for (uint32_t i = 0; i < p[1] && i < RANGES && 2 + 8 * i + 8 <= n; i++) {
            const uint8_t* q = p + 2 + 8 * i;
            s_range[i][0] = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
            s_range[i][1] = (uint32_t)q[4] | (uint32_t)q[5] << 8 | (uint32_t)q[6] << 16 | (uint32_t)q[7] << 24;
            s_ranges = (uint8_t)(i + 1);
        }
        return 0;
    case 'G': s_ready = 1; return 0;
    }
    return 1;
}
#else
// ---- the card, from the module's flash
extern const uint32_t memcard_blocks, memcard_lba[];
extern const uint16_t memcard_count;
extern const uint8_t memcard_data[][512];
static int memRead(void* ctx, uint32_t lba, uint8_t* dst)
{
    (void)ctx;
    uint16_t lo = 0, hi = memcard_count;
    while (lo < hi) { const uint16_t m = (uint16_t)((lo + hi) / 2); if (memcard_lba[m] < lba) lo = (uint16_t)(m + 1); else hi = m; }
    const uint8_t* src = lo < memcard_count && memcard_lba[lo] == lba ? memcard_data[lo] : 0;
    for (int i = 0; i < 512; i++) dst[i] = src ? src[i] : 0;
    return 0;
}
#endif
void    hal_sd_select(int on) { sd_emu_select(&s_card, on); }
uint8_t hal_spi(uint8_t out) { return sd_emu_xfer(&s_card, out); }
void    hal_spi_fast(int on) { (void)on; }

// ---- the screen: an SSD1306 as far as fw/ui.c can tell
// fw/ui.c draws 8 pages of 128 x 8 for the OLED; here they're laid out on the 240 x 240 colour panel:
//   deck   pages 0-3 (the name, time, BPM, state) 15 px each, white
//          pages 4-6 (the scrolling waveform) 30 px each, coloured by loudness, dimmed behind the playhead
//          the overview drawn from g_app.ov (not page 7): 44 px, orange ahead, dim behind, playhead, cue
//   list   all 8 pages 25 px each, white
//   and the top line: the name, the CPU meter (header())
#define SCALE_W 240
#define WAVE_X  32                                // fw/ui.c's playhead column
static uint8_t  s_oled[UI_PAGES][128];
static uint8_t  s_dirty = 0xFF;                   // a bit a page
static uint8_t  s_curPage;
static uint8_t  s_layout = 0xFF;                  // 0 deck, 1 list
// The panel is on a 12 MHz SPI with no vsync (fm1-dock doesn't turn on its TE): a waveform band (240 x 30, 14 KB)
// takes ~10 ms to go, the overview ~14, and fm1-dock's blit() waits until all of it has gone. The main loop
// is also where the blocks from the computer are taken, and fm1-dock holds one frame of them at a time (the
// next one that starts coming in meanwhile, ~1 ms later, is dropped): so a band is drawn here whole, then sent STRIP
// rows a pass, and the next band only once it has all gone.
#define STRIP 2u                          // ~0.6 ms: under the ~1 ms between two frames from the computer
static uint16_t s_px[SCALE_W * 44];               // the tallest band, as the panel takes it
static uint32_t s_maxWork, s_maxPut;              // the longest main-loop pass, and page put, this second (ticks)
static int32_t  s_sendY;                          // the band being sent: its top on the panel,
static uint32_t s_sendH, s_sendRow;               //  its height, the rows gone
static void blit(int32_t y0, uint32_t h) { s_sendY = y0; s_sendH = h; s_sendRow = 0; }
static int lcdFree(void) { return s_sendRow >= s_sendH; }
static void pump(void)                            // the next strip of the band being sent
{
    if (lcdFree()) return;
    const uint32_t k = s_sendH - s_sendRow < STRIP ? s_sendH - s_sendRow : STRIP;
    H->blit(0, s_sendY + (int32_t)s_sendRow, SCALE_W, (int32_t)k, s_px + s_sendRow * SCALE_W);
    s_sendRow += k;
}

void hal_i2c_send(uint8_t addr, const uint8_t* p, uint16_t n)
{
    (void)addr;
    if (!n) return;
    if (p[0] == 0x00) {                           // commands: only the page address matters here
        for (uint16_t i = 1; i + 1 < n; i++)
            if (p[i] == 0x22) { s_curPage = (uint8_t)(p[i + 1] & 7); i += 2; }
            else if (p[i] == 0x21) i += 2;
        return;
    }
    if (p[0] != 0x40) return;                     // data: one page
    uint8_t* row = s_oled[s_curPage];
    uint8_t changed = 0;
    for (uint16_t i = 1; i < n && i <= 128; i++)
        if (row[i - 1] != p[i]) { row[i - 1] = p[i]; changed = 1; }
    if (changed) s_dirty |= (uint8_t)(1u << s_curPage);
}
int hal_i2c_busy(void) { return 0; }

static inline uint16_t be(uint16_t c) { return (uint16_t)(c << 8 | c >> 8); }   // the panel's byte order
static const uint8_t kSrcX[SCALE_W] = {
#define X8(b) (uint8_t)(((b) + 0) * 8 / 15), (uint8_t)(((b) + 1) * 8 / 15), (uint8_t)(((b) + 2) * 8 / 15), (uint8_t)(((b) + 3) * 8 / 15), \
              (uint8_t)(((b) + 4) * 8 / 15), (uint8_t)(((b) + 5) * 8 / 15), (uint8_t)(((b) + 6) * 8 / 15), (uint8_t)(((b) + 7) * 8 / 15)
#define X40(b) X8(b), X8((b) + 8), X8((b) + 16), X8((b) + 24), X8((b) + 32)
    X40(0), X40(40), X40(80), X40(120), X40(160), X40(200)
};

// a page scaled to 240 x h, one colour
static void putText(uint8_t page, int32_t y0, uint8_t h)
{
    const uint16_t ink = be(HB_RGB(235, 235, 235));
    for (uint8_t y = 0; y < h; y++) {
        const uint8_t bit = (uint8_t)(y * 8 / h);
        uint16_t* d = s_px + y * SCALE_W;
        for (uint16_t x = 0; x < SCALE_W; x++) d[x] = s_oled[page][kSrcX[x]] >> bit & 1 ? ink : 0;
    }
    blit(y0, h);
}

// a waveform page (4..6) at 30 px: the colour from how far the row is from the middle of the three
static const uint16_t kWave[12] = {               // 0 = the middle .. 11 = the peaks: deep blue, cyan, white
    HB_RGB(0, 60, 170), HB_RGB(0, 80, 195), HB_RGB(0, 100, 215), HB_RGB(0, 125, 235), HB_RGB(10, 150, 250), HB_RGB(30, 175, 255),
    HB_RGB(60, 195, 255), HB_RGB(100, 210, 255), HB_RGB(140, 225, 255), HB_RGB(180, 238, 255), HB_RGB(215, 247, 255), HB_RGB(245, 252, 255),
};
// The waveform: 90 rows, mirrored about its middle. Its three pages are taken together as ui.c has them (s_wave),
// drawn into s_wpx a band a pass, then sent from the middle out, a row and its mirror a pass: the two halves of
// a column change within a fraction of a millisecond (sent top to bottom, the panel showed the top half of the
// new picture over the bottom half of the old one for ~30 ms).
#define WAVE_Y (HB_TOP + 66)
#define WAVE_H 90u
static uint8_t  s_wave[3][128];
static uint16_t s_wpx[SCALE_W * WAVE_H];
static uint8_t  s_wState;                         // 0 idle, 1..3 band (s_wState - 1) to draw, 4 sending
static uint8_t  s_wPair;                          // the next row pair to send: rows 44 - p and 45 + p
static void putWave(uint8_t page, int32_t y0)
{
    (void)y0;
    const uint8_t h = 30;
    const uint8_t* src = s_wave[page - 4];
    uint16_t* const out = s_wpx + (uint32_t)(page - 4) * h * SCALE_W;
    for (uint8_t y = 0; y < h; y++) {
        const uint8_t bit = (uint8_t)(y * 8 / h);
        const int32_t r2 = 2 * ((page - 4) * 8 + bit) - 23;                    // twice the distance from the middle
        const uint8_t lvl = (uint8_t)((r2 < 0 ? -r2 : r2) / 2);
        const uint16_t on = be(kWave[lvl > 11 ? 11 : lvl]);
        const uint16_t c = kWave[lvl > 11 ? 11 : lvl];
        const uint16_t dim = be((uint16_t)((c >> 1) & 0x7BEF));                // half bright: behind the playhead
        uint16_t* d = out + y * SCALE_W;
        for (uint16_t x = 0; x < SCALE_W; x++) {
            const uint8_t sx = kSrcX[x];
            if (sx == WAVE_X) { d[x] = be(HB_RGB(255, 255, 255)); continue; }   // the playhead
            uint8_t b = src[sx];
            if (sx < WAVE_X) b |= src[sx ^ 1];                                  // ui.c checkers the played part: fill it in
            d[x] = b >> bit & 1 ? (sx < WAVE_X ? dim : on) : 0;
        }
    }
}
static int waveStep(void)                          // 1: it used the pass (drew a band or sent a pair)
{
    if (s_wState == 0) {
        if (s_layout != 0 || !(s_dirty & 0x70) || !lcdFree()) return 0;
        for (uint8_t b = 0; b < 3; b++) for (uint8_t x = 0; x < 128; x++) s_wave[b][x] = s_oled[4 + b][x];
        s_dirty &= (uint8_t)~0x70u; s_wState = 1;
    }
    if (s_wState <= 3) { putWave((uint8_t)(4 + s_wState - 1), 0); s_wState++; s_wPair = 0; return 1; }
    const uint32_t a = WAVE_H / 2 - 1 - s_wPair, b = WAVE_H / 2 + s_wPair;
    H->blit(0, WAVE_Y + (int32_t)a, SCALE_W, 1, s_wpx + a * SCALE_W);
    H->blit(0, WAVE_Y + (int32_t)b, SCALE_W, 1, s_wpx + b * SCALE_W);
    if (++s_wPair >= WAVE_H / 2) s_wState = 0;
    return 1;
}

// the overview, from the track's 128 columns of 0..15
static void putOverview(int32_t y0)
{
    const uint8_t h = 44, mid = 22;
    const uint32_t frames = g_app.frames;
    const uint32_t per = frames >= 240 ? frames / 240 : 1;
    const uint32_t head = frames >= 240 ? (uint32_t)lite_pos(&g_deck) / per : 0, cue = g_app.cue / per;
    for (uint16_t i = 0; i < SCALE_W * h; i++) s_px[i] = 0;
    if (frames >= 240 && g_app.track != APP_NONE)
        for (uint16_t x = 0; x < SCALE_W; x++) {
            const uint8_t col = (uint8_t)(x * 128 / SCALE_W);
            const uint8_t v = (g_app.ov[col >> 1] >> ((col & 1) << 2)) & 15;
            const uint8_t half = (uint8_t)(1 + v * (mid - 2) / 15);
            const int played = x < head;
            for (uint8_t y = (uint8_t)(mid - half); y <= mid + half && y < h; y++) {
                const uint8_t edge = (uint8_t)(y < mid ? mid - y : y - mid);       // brighter at the middle
                const uint16_t c = played ? HB_RGB(110, 70, 40) : edge * 3 < half ? HB_RGB(255, 200, 90) : HB_RGB(255, 140, 30);
                s_px[y * SCALE_W + x] = be(c);
            }
            if (x == cue || x == cue + 1)                                          // the cue: a yellow line
                for (uint8_t y = 0; y < h; y++) s_px[y * SCALE_W + x] = be(HB_RGB(255, 230, 0));
        }
    if (frames >= 240 && head < SCALE_W)
        for (uint8_t y = 0; y < h; y++) s_px[y * SCALE_W + head] = s_px[y * SCALE_W + (head + 1 < SCALE_W ? head + 1 : head)] = be(HB_RGB(255, 255, 255));
    blit(y0, h);
}

static void putPage(uint8_t page)
{
    if (s_layout == 1) { putText(page, HB_TOP + 2 + page * 25, 25); return; }   // (all 8 from s_oled)
    if (page < 4) putText(page, HB_TOP + 2 + page * 15, 15);
    else if (page < 7) {}                         // (waveStep())
    else putOverview(HB_TOP + 162);
}
static void layout(void)                          // the deck or the list: all of it again when it changes
{
    const uint8_t l = g_app.browsing ? 1 : 0;
    if (l == s_layout) return;
    s_layout = l;
    s_sendRow = s_sendH;                          // (a band half sent: dropped, all is drawn again)
    H->fill(0, HB_TOP, 240, 240 - HB_TOP, 0);
    s_dirty = 0xFF; s_wState = 0;
}

// ---- audio: blocks into the ring here; render() takes them at half its rate
// Two counters, one writer each: the main loop counts blocks written, render() blocks played.
static uint8_t s_ring[HAL_AUDIO_BLOCKS][2 * HAL_AUDIO_BLOCK];
static volatile uint32_t s_wrote, s_played;
static uint8_t s_pos;                             // sample within the block being played
static int32_t s_prev;
static volatile uint32_t s_under;

uint8_t* hal_audio_block(void)
{
    return s_wrote - s_played < HAL_AUDIO_BLOCKS ? s_ring[s_wrote % HAL_AUDIO_BLOCKS] : 0;
}
void hal_audio_commit(void) { s_wrote++; }

static void render(int32_t* out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 2) {
        int32_t s = s_prev;
        if (s_wrote != s_played) {
            const uint8_t* b = s_ring[s_played % HAL_AUDIO_BLOCKS] + 2 * s_pos;
            s = (int32_t)((uint16_t)(b[0] << 8 | b[1])) - 32768;
            if (++s_pos == HAL_AUDIO_BLOCK) { s_pos = 0; s_played++; }
        } else {
            s_under++;
            s -= s >> 4;                          // ran dry: glide to silence, no click
        }
        const int32_t mid = (s_prev + s) >> 1;    // 22050 -> 44100, linear
        out[2 * i] += mid; out[2 * i + 1] += mid;
        if (i + 1 < n) { out[2 * i + 2] += s; out[2 * i + 3] += s; }
        s_prev = s;
    }
}

// ---- the module
static void lights(void)
{
    static uint8_t shown = 0xFF;
    const uint8_t st = g_app.state;
    if (st == shown) return;
    shown = st;
    H->led(HB_BTN_PLAY, st == ST_PLAY);           // lit or off (fm1-dock's glow, 2, is a fast pulse that fm1-emu
    H->led(HB_BTN_SEQ, st == ST_CUE);             //  shows as a flicker)
}

static void work(uint32_t full)
{
    if (full) s_layout = 0xFF;                    // layout() clears and redraws it all
#if FM1_HOST
    static uint32_t helloAt, statAt;
    if (!s_started) {                             // the computer first: the card's pinned blocks
        if (!s_ready) {
            if (full || H->ms() - helloAt > 1000) {
                helloAt = H->ms();
                H->midi_send(0xBF, 107, 1);
                H->fill(0, 100, 240, 40, 0);
                H->text(0, 100, 240, 0, "WAITING FOR THE HOST", HB_RGB(235, 235, 235), 1);
                H->text(0, 120, 240, 0, "tools/fm1host.py", HB_RGB(40, 170, 255), 1);
            }
            return;
        }
        H->fill(0, HB_TOP, 240, 240 - HB_TOP, 0);
        s_started = 1;
        app_init();
    }
    standing();
    holes();
    static uint32_t passes;                       // main-loop passes a second and misses, to the computer: CC 109 / 110 / 111
    passes++;
    if (H->ms() - statAt >= 1000) {
        statAt = H->ms();
        H->midi_send(0xBF, 109, passes & 0x7F); H->midi_send(0xBF, 110, passes >> 7 & 0x7F);
        H->midi_send(0xBF, 111, s_miss > 127 ? 127 : s_miss);
        const uint32_t w = s_maxWork / 2400u, q = s_maxPut / 2400u;   // the longest pass, and page, in 0.1 ms
        H->midi_send(0xBF, 114, w > 127 ? 127 : w); H->midi_send(0xBF, 115, q > 127 ? 127 : q);
        s_maxWork = s_maxPut = 0;
        passes = 0; s_miss = 0;                   // (misses: this last second)
    }
#endif
    app_step();
    layout();
#if FM1_HOST
    const uint8_t perPass = 1;                    // one page a pass: the blocks from the computer come between passes
#else
    const uint8_t perPass = 2;
#endif
    const uint32_t t = H->ticks();
    if (!waveStep())                              // the waveform first, whole; the other pages between
        for (uint8_t k = 0, put = 0; k < UI_PAGES && put < perPass && lcdFree(); k++) {   // (the panel busy: next pass)
            if (s_layout == 0 && k >= 4 && k <= 6) continue;
            if (!(s_dirty & (1u << k))) continue;
            s_dirty &= (uint8_t)~(1u << k);
            putPage(k); put++;                    // (drawn into s_px; pump() sends it)
            app_step();
        }
    pump();
    const uint32_t d = H->ticks() - t;
    if (d > s_maxPut) s_maxPut = d;
    lights();
    static uint32_t logged, logAt;                // the ring running dry, in the host's log (tools/dock.py log)
    if (s_under != logged && H->ms() - logAt > 1000) { logged = s_under; logAt = H->ms(); H->log_u32("freedj: dry samples ", logged); }
}

// The top line is the deck's (HB_F_SCREEN): its name and the CPU meter, the share of the time the deck's
// main-loop work takes (decoding, keylock, the screen; fm1-dock's own "%" there would count only the
// audio interrupt, ~0.4 % here), measured on the 24 MHz counter, every second.
#if FM1_HOST
#define DECK_NAME "FREEDJ NANO"
#else
#define DECK_NAME "FREEDJ NANO"
#endif
static uint32_t s_busy, s_meterAt, s_cpu = 0xFF, s_hdr;
// The CPU figure alone, on a canvas off screen (60 x 17, ~1.4 ms to the panel): clearing the line and writing it
// again on the panel showed it blank between (a flicker), and the whole line took ~8 ms, holding up the main loop.
#define CPU_W 60
static void cpuText(void)
{
    char t[12] = "CPU ";
    uint32_t i = 4, v = s_cpu > 999 ? 999 : s_cpu;
    if (s_cpu == 0xFF) t[i++] = '-';
    else { if (v >= 100) t[i++] = (char)('0' + v / 100); if (v >= 10) t[i++] = (char)('0' + v / 10 % 10); t[i++] = (char)('0' + v % 10); }
    t[i++] = '%'; t[i] = 0;
    const uint16_t c = s_cpu == 0xFF ? HB_RGB(120, 120, 120) : s_cpu >= 80 ? HB_RGB(255, 70, 50)
                     : s_cpu >= 50 ? HB_RGB(255, 210, 0) : HB_RGB(80, 220, 110);
    H->cv_begin(CPU_W, 17, 0);
    H->cv_text(CPU_W - 4 - H->text_w(0, t), 0, 0, t, c);   // right-aligned, 4 px in from the edge
    H->cv_blit(240 - CPU_W, 2);
}
static void header(void)                          // all of the line: on a full redraw only
{
    H->fill(0, 0, 240, HB_TOP - 1, 0);
    H->text(4, 2, 140, 0, DECK_NAME, HB_RGB(170, 140, 255), 0);
    H->fill(0, HB_TOP - 2, 240, 1, HB_RGB(90, 90, 90));
    cpuText();
}
static void meter(void)
{
    const uint32_t now = H->ticks(), span = now - s_meterAt;
    if (span < 24000000u) return;                 // a second
    const uint32_t pct = s_busy / (span / 100u);
    s_busy = 0; s_meterAt = now;
#if FM1_HOST
    if (s_started) H->midi_send(0xBF, 113, pct > 127 ? 127 : pct);
#endif
    if (s_cpu != 0xFF && pct + 1 >= s_cpu && pct <= s_cpu + 1) return;   // +-1: as shown (else it flips at idle)
    s_cpu = pct;
    s_hdr = 1;
}
static void draw(uint32_t full)
{
    const uint32_t t0 = H->ticks();
    work(full);
    if (full) header();                           // (after work(): a full redraw clears the screen first)
    else if (s_hdr && lcdFree() && !s_wState) { s_hdr = 0; cpuText(); }   // (its text would wait for a band going)
    const uint32_t d = H->ticks() - t0;
    s_busy += d;
    if (d > s_maxWork) s_maxWork = d;
    meter();
}

static int init(const hb_host_t* h)
{
    H = h;
    if (!HB_HOST_HAS(h, blit)) return 1;
    s_pins = 0; s_jog = 0; s_pot = 512; s_wrote = s_played = 0; s_pos = 0; s_prev = 0; s_dirty = 0xFF; s_layout = 0xFF; s_busy = 0; s_meterAt = h->ticks(); s_cpu = 0xFF;
    s_card.read = memRead;
#if FM1_HOST
    s_card.blocks = s_blocks;
    s_pins_n = 0; s_ranges = 0; s_next = 0; s_pinning = s_ready = s_started = 0;
    s_reqFrom = s_reqEnd = 0; s_miss = s_got = 0; s_jumpLba = s_jumpAt = 0; s_front = 0;
    for (int i = 0; i < 4; i++) s_holeLba[i] = 0; for (uint32_t i = 0; i < SIDE; i++) s_sideTag[i] = 0;
    for (int i = 0; i < 4; i++) s_askedLba[i] = 0;
    for (uint32_t i = 0; i < CACHE; i++) s_tag[i] = s_halfTag[i] = 0;
#else
    s_card.blocks = memcard_blocks;
#endif
    sd_emu_reset(&s_card);
    s_card.access = 2; s_card.gap = 1;            // RAM answers at once
#if !FM1_HOST
    app_init();
#endif
    return 0;
}

static void release(void) {}

HB_DESC = {
    .abi = HB_ABI, .name = "FREEDJ", .init = init, .release = release,
    .render = render, .ui = ui, .draw = draw, .midi = midi,
#if FM1_HOST
    .flags = HB_F_PANEL | HB_F_SCREEN | HB_F_DATA, .data = data,
#else
    .flags = HB_F_PANEL | HB_F_SCREEN,
#endif
};
