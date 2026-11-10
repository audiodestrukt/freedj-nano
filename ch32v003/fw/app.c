// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "hal.h"
#include "ui.h"

// Feel (cf. components/nano deck.cpp)
#define NUDGE_PER_COUNT  328      // Q16: 0.5 % per encoder count while playing
#define NUDGE_MAX        13107    // 20 %
#define SEARCH_PER_COUNT 88       // frames (4 ms) per encoder count while paused
#define SCRUB_MAX        0x40000  // Q16: scrubbing plays at up to 4x either way
#define SCRUB_IDLE_MS    80       // the jog still this long and caught up: the scrub ends
#define BROWSE_IDLE_MS   8000
#define POT_DEAD         12       // ADC counts either side of the centre that read 0 %

app_view  g_app;
lite_deck g_deck;

enum { P_NONE, P_CUE, P_LOAD, P_SCRUB };   // waiting for the fade out to finish
static trk_info s_trk;
static uint8_t  s_pending;
static int32_t  s_nudge, s_tempoQ16;
static int16_t  s_fader;          // tempo control, -500..500 of the range
static uint32_t s_browseAt, s_retryAt;
static uint8_t  s_scrub;          // paused, and the jog is playing the audio under it
static int32_t  s_scrubTo;        // where the jog has put the platter
static uint32_t s_jogAt;
static const uint8_t kRanges[] = { 6, 10, 16, 50 };

// ---- deck

static void rd(void* ctx, uint32_t off, uint8_t* dst, uint8_t n) { (void)ctx; trk_read(s_trk.off + off, dst, n); }

static void applySpeed(void) { lite_set_speed(&g_deck, 0x10000 + s_tempoQ16 + s_nudge); }

static void setFader(int16_t f)
{
    s_fader = f < -500 ? -500 : f > 500 ? 500 : f;
    g_app.tempo = (int16_t)(s_fader * g_app.range / 50);              // 1/10 %
    s_tempoQ16 = ((int32_t)g_app.tempo * 8389) >> 7;                  // x 65536 / 1000
    applySpeed();
}

static void loadNow(uint16_t i)
{
    if (!trk_get(i, &s_trk)) return;
    g_app.track = i; g_app.frames = s_trk.frames; g_app.bpmX100 = s_trk.bpmX100; g_app.firstBeat = s_trk.firstBeat;
    g_app.peaksOff = s_trk.peaksLen >= (s_trk.frames + TRK_PEAK - 1) / TRK_PEAK ? s_trk.peaksOff : 0;
    g_app.cue = 0; g_app.state = ST_STOP;
    trk_name(i, g_app.name);
    trk_overview(i, g_app.ov);
    const lite_src src = { rd, 0, s_trk.frames };
    lite_init(&g_deck, &src);
    lite_set_keylock(&g_deck, g_app.keylock);
    applySpeed();
}

// The scrub hands the deck back: keylock and tempo as they were.
static void endScrub(void)
{
    if (!s_scrub) return;
    s_scrub = 0;
    lite_set_keylock(&g_deck, g_app.keylock);
    applySpeed();
}

static void stop(void) { endScrub(); g_app.state = ST_STOP; lite_play(&g_deck, 0); }

static void load(uint16_t i)
{
    g_app.browsing = 0;
    if (i >= g_app.count) return;
    g_app.sel = i;
    if (g_deck.playing) { stop(); s_pending = P_LOAD; }
    else loadNow(i);
}

static void play(int pressed)
{
    if (!pressed || g_app.track == APP_NONE) return;
    if (s_scrub) { endScrub(); g_app.state = ST_PLAY; return; }      // play on from the scrub
    if (g_app.state == ST_CUE) g_app.state = ST_PLAY;                 // PLAY during a preview: keep going
    else if (g_app.state == ST_PLAY) stop();
    else { g_app.state = ST_PLAY; s_pending = P_NONE; lite_play(&g_deck, 1); }
}

static void cue(int pressed)
{
    if (g_app.track == APP_NONE) return;
    if (pressed) {
        if (g_app.state == ST_PLAY) { stop(); s_pending = P_CUE; }     // back to the cue, paused
        else {
            endScrub();
            const uint32_t p = (uint32_t)lite_pos(&g_deck);
            if (p != g_app.cue) g_app.cue = p;                         // set it here
            g_app.state = ST_CUE; s_pending = P_NONE;
            lite_play(&g_deck, 1);                                     // preview while held
        }
    } else if (g_app.state == ST_CUE) {
        stop(); s_pending = P_CUE;
    }
}

static void jog(int16_t d)
{
    if (g_app.track == APP_NONE || !d) return;
    if (g_app.state == ST_STOP) {                                      // scrub: play what's under the jog
        if (g_deck.playing && !s_scrub) return;                        // still fading out
        if (!s_scrub) {
            s_scrub = 1;
            s_scrubTo = lite_pos(&g_deck);
            lite_set_keylock(&g_deck, 0);                              // varispeed, both ways
            lite_set_speed(&g_deck, 0);
            lite_play(&g_deck, 1);
        }
        s_scrubTo += d * SEARCH_PER_COUNT;
        if (s_scrubTo < 0) s_scrubTo = 0;
        if (s_scrubTo >= (int32_t)g_app.frames) s_scrubTo = (int32_t)g_app.frames - 1;
        s_jogAt = hal_ms();
    } else {
        s_nudge += d * NUDGE_PER_COUNT;
        if (s_nudge > NUDGE_MAX) s_nudge = NUDGE_MAX;
        if (s_nudge < -NUDGE_MAX) s_nudge = -NUDGE_MAX;
        applySpeed();
    }
}

static void keylock(void) { g_app.keylock ^= 1; lite_set_keylock(&g_deck, g_app.keylock); }

static void range(void)
{
    uint8_t i = 0;
    while (i + 1u < sizeof kRanges && kRanges[i] != g_app.range) i++;
    g_app.range = kRanges[i + 1u < sizeof kRanges ? i + 1 : 0];
    setFader(s_fader);
}

static void browse(int16_t d)
{
    s_browseAt = hal_ms();
    if (!g_app.browsing) { g_app.browsing = 1; g_app.sel = g_app.track == APP_NONE ? 0 : g_app.track; return; }
    int32_t s = (int32_t)g_app.sel + d;
    g_app.sel = (uint16_t)(s < 0 ? 0 : s >= g_app.count ? g_app.count - 1 : s);
}

// ---- panel: EC11, buttons, pot

static uint8_t  s_ab, s_stable, s_last;
static int8_t   s_enc;
static uint32_t s_tick;
static uint16_t s_pot = 0xFFFF;
// (previous AB << 2 | AB): +1 clockwise, -1 counter-clockwise, 0 no move or a bounce
static const int8_t kQuad[16] = { 0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0 };

static void push(void)
{
    if (g_app.browsing) load(g_app.sel);
    else if (s_stable & PIN_SHIFT) range();
    else keylock();
}

static void panel(void)
{
    const uint8_t p = hal_pins();
    const uint8_t ab = p & (PIN_ENC_A | PIN_ENC_B);
    if (ab != s_ab) { s_enc += kQuad[s_ab << 2 | ab]; s_ab = ab; }

    const uint32_t now = hal_ms();
    if (now - s_tick < 2) return;                 // buttons and pot every 2 ms
    s_tick = now;
    const uint8_t b = p & ~(PIN_ENC_A | PIN_ENC_B);
    if (b == s_last) {                            // the same twice running: debounced
        const uint8_t on = b & ~s_stable, off = s_stable & ~b;
        s_stable = b;
        if ((on | off) & PIN_PLAY) play(!!(on & PIN_PLAY));
        if ((on | off) & PIN_CUE) cue(!!(on & PIN_CUE));
        if (on & PIN_PUSH) push();
    }
    s_last = b;

    if (s_stable & PIN_SHIFT) {                   // one list row per detent (4 counts)
        while (s_enc >= 4) { browse(1); s_enc -= 4; }
        while (s_enc <= -4) { browse(-1); s_enc += 4; }
    } else if (s_enc) {
        jog(s_enc); s_enc = 0;
    }

    const uint16_t v = hal_pot();
    if (s_pot == 0xFFFF || v > s_pot + 3 || v + 3 < s_pot) {
        s_pot = v;
        int16_t c = (int16_t)v - 512;
        c = c > POT_DEAD ? c - POT_DEAD : c < -POT_DEAD ? c + POT_DEAD : 0;
        setFader(c);
    }
}

// ---- the bridge: 0x01 + a USB-MIDI packet (DJ2Go map), else console keys

static uint8_t s_shift[2], s_shiftUsed[2];

static void midi(uint8_t st, uint8_t d1, uint8_t d2)
{
    const uint8_t kind = st & 0xF0;
    const int on = kind == 0x90 && d2, off = kind == 0x80 || (kind == 0x90 && !d2);
    if (on || off) {
        const uint8_t dk = d1 == 0x4B ? 0 : 1;
        switch (d1) {
        case 0x4B: case 0x34:                     // LOAD A / B: also a shift
            if (on) { s_shift[dk] = 1; s_shiftUsed[dk] = 0; }
            else { s_shift[dk] = 0; if (!s_shiftUsed[dk]) load(g_app.sel); }
            return;
        case 0x5A: if (off) { if (g_app.browsing) load(g_app.sel); else browse(0); } return;   // ENTER
        case 0x59: if (off) g_app.browsing = 0; return;                  // BACK
        }
        if (on) s_shiftUsed[0] = s_shift[0], s_shiftUsed[1] = s_shift[1];
        switch (d1) {
        case 0x3B: case 0x42: play(on); break;
        case 0x33: case 0x3C: cue(on); break;
        case 0x43: case 0x45: if (on) keylock(); break;                  // pitch bend -
        case 0x44: case 0x46: if (on) range(); break;                    // pitch bend +
        }
        return;
    }
    if (kind != 0xB0) return;
    s_shiftUsed[0] |= s_shift[0]; s_shiftUsed[1] |= s_shift[1];
    const int16_t rel = d2 < 64 ? d2 : (int16_t)d2 - 128;
    switch (d1) {
    case 0x19: case 0x18: jog(rel); break;
    case 0x1A: browse(rel); break;
    case 0x0D: case 0x0E:                         // tempo faders, top = slower; the detent reads 63 or 64
        setFader(d2 == 63 || d2 == 64 ? 0 : (int16_t)((64 - (int16_t)d2) * 500 / 63));
        break;
    }
}

static void key(uint8_t c)
{
    switch (c) {
    case 'p': play(1); play(0); break;
    case 'c': cue(1); cue(0); break;
    case 'k': keylock(); break;
    case '+': setFader(s_fader + 500 / g_app.range); break;      // 1 %
    case '-': setFader(s_fader - 500 / g_app.range); break;
    case '0': setFader(0); break;
    case 'r': range(); break;
    case 'n': load(g_app.track == APP_NONE ? 0 : g_app.track + 1); break;
    case 'b': load(g_app.track == APP_NONE || !g_app.track ? 0 : g_app.track - 1); break;
    case 'j': jog(-8); break;
    case 'J': jog(8); break;
    case 'l': if (g_app.browsing) g_app.browsing = 0; else browse(0); break;
    }
}

static uint8_t s_pkt[4];
static int8_t  s_pktN = -1;

static void serial(void)
{
    int c;
    for (uint8_t n = 0; n < 16 && (c = hal_uart_rx()) >= 0; n++) {
        if (s_pktN >= 0) {
            s_pkt[s_pktN++] = (uint8_t)c;
            if (s_pktN == 4) {
                s_pktN = -1;
                const uint8_t cin = s_pkt[0] & 15;
                if (cin == 0x8 || cin == 0x9 || cin == 0xB) midi(s_pkt[1], s_pkt[2], s_pkt[3]);
            }
        } else if (c == 0x01) {
            s_pktN = 0;
        } else {
            key((uint8_t)c);
        }
    }
}

// ---- main loop

static void mount(void)
{
    g_app.err = (uint8_t)trk_mount(ui_borrow());          // Petit FatFs's state, only while mounting
    g_app.count = trk_count();
    if (!g_app.err && g_app.count) loadNow(0);
    else s_retryAt = hal_ms() + 2000;
}

static uint8_t s_mounted;

void app_init(void)
{
    g_app.track = APP_NONE;
    g_app.range = 10;
    ui_begin();                                   // the mount waits for this to go out
}

static void afterBlock(void)
{
    if (s_scrub) {                                // chase the jog: cover the gap in ~23 ms
        const int32_t gap = s_scrubTo - lite_pos(&g_deck);
        int32_t sp = gap > SCRUB_MAX >> 7 ? SCRUB_MAX : gap < -(SCRUB_MAX >> 7) ? -SCRUB_MAX : gap << 7;
        lite_set_speed(&g_deck, sp);
        if (gap > -16 && gap < 16 && hal_ms() - s_jogAt > SCRUB_IDLE_MS) {         // caught up, jog still:
            stop(); s_pending = P_SCRUB;                                            // land exactly on it
        }
    }
    if (s_nudge) {                                // nudge springs back (~50 ms)
        s_nudge -= s_nudge >> 6;
        if (s_nudge > -64 && s_nudge < 64) s_nudge = 0;
        applySpeed();
    }
    if (s_pending && !g_deck.playing) {           // faded out
        if (s_pending == P_CUE) lite_seek(&g_deck, (int32_t)g_app.cue);
        else if (s_pending == P_SCRUB) lite_seek(&g_deck, s_scrubTo);
        else loadNow(g_app.sel);
        s_pending = P_NONE;
    }
    if (g_app.state != ST_STOP && lite_pos(&g_deck) >= (int32_t)g_app.frames) stop();   // the end
}

void app_step(void)
{
    uint8_t* b;
    while ((b = hal_audio_block())) {
        int16_t s[HAL_AUDIO_BLOCK];
        lite_render(&g_deck, s, HAL_AUDIO_BLOCK);
        for (uint8_t k = 0; k < HAL_AUDIO_BLOCK; k++) lite_pwm(s[k], &b[2 * k], &b[2 * k + 1]);
        hal_audio_commit();
        afterBlock();
    }
    panel();
    serial();
    ui_step();
    const uint32_t now = hal_ms();
    if ((!s_mounted || g_app.err) && (int32_t)(now - s_retryAt) >= 0 && !hal_i2c_busy()) {
        s_mounted = 1;
        mount();
    }
    if (g_app.browsing && now - s_browseAt > BROWSE_IDLE_MS) g_app.browsing = 0;
}
