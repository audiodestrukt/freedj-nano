// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// freedj-nano on one core: an audio task (high priority, renders the deck into
// I2S DMA) and this control loop (inputs, MIDI, touch, UI) at low priority.
//
// Controls (any board):
//   encoder         jog: nudge while playing, scrub while stopped
//   KEY + encoder   tempo (no pot) ; KEY tap: keylock on/off
//   push / LIST     open the track list; in the list, encoder browses and push loads
//   PLAY, CUE       CDJ-style
// Bench console (USB serial, no hardware needed): p play, c cue tap, k keylock,
// + / - tempo 1 %, 0 tempo 0, r range, n / b next / previous track, j / J jog -/+, l list, ? help
// MIDI from a controller on the computer (freedj-esp's tools/midi_bridge.py): 0x01 + a 4-byte
// USB-MIDI packet, decoded with the DJ2Go map as if the controller were plugged in here.
#include <stdio.h>
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw.h"
#include "nano/deck.h"
#include "nano/dj2go.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

static const char* TAG = "nano";
using deck::Type;

static nano::Library s_lib;
static nano::Deck* s_deck;
static nano::Track s_track;
static int s_trackIdx = -1, s_selected = 0, s_tempo = 500;
static bool s_browsing, s_keyUsed;
static int s_browseAcc;
static dj2go::Decoder s_dj;
static volatile int64_t s_busyUs, s_audioFrames;

static deck::Event make(Type t, bool pressed = true, int16_t delta = 0, float pos = 0)
{
    deck::Event e; e.type = t; e.deck = deck::DeckA; e.pressed = pressed; e.delta = delta; e.position = pos; return e;
}

static void loadSelected(int index)
{
    if (index < 0 || index >= s_lib.count()) return;
    s_deck->post(make(Type::Load, true, (int16_t)index));
    s_browsing = false;
}

// App-level routing for every source: the track list lives here, the rest goes to the deck.
static void handle(const deck::Event& e, bool fromTouch)
{
    switch (e.type) {
    case Type::Browse:
        if (fromTouch || !s_browsing) {               // touch LIST toggles; a first turn opens the list
            s_browsing = fromTouch ? !s_browsing : true;
            if (s_browsing) s_selected = s_trackIdx < 0 ? 0 : s_trackIdx;
            return;
        }
        s_selected += e.delta;
        if (s_selected < 0) s_selected = 0;
        if (s_selected >= s_lib.count()) s_selected = s_lib.count() - 1;
        return;
    case Type::Load:
        if (e.pressed) loadSelected(fromTouch ? e.delta : s_selected);
        return;
    case Type::Back: s_browsing = false; return;
    case Type::Shift: return;
    default: s_deck->post(e);
    }
}

static void onMidi(uint8_t status, uint8_t d1, uint8_t d2)
{
    deck::Event ev[2];
    const int n = s_dj.decode(status, d1, d2, ev, 2);   // DJ2Go map; both decks drive the one deck
    for (int i = 0; i < n; i++) handle(ev[i], false);
}

static void controls()
{
    using namespace inputs;
    State s;
    poll(s);
    if (s.pressed[Key]) s_keyUsed = false;
    if (s.encoder) {
        if (s.down[Key]) {                                 // tempo: 2 fader units per count
            s_tempo += 2 * s.encoder;
            s_tempo = s_tempo < 0 ? 0 : s_tempo > 1000 ? 1000 : s_tempo;
            s_deck->post(make(Type::TempoFader, false, 0, s_tempo / 1000.0f));
            s_keyUsed = true;
        } else if (s_browsing) {
            s_browseAcc += s.encoder;                      // one row per detent
            if (s_browseAcc / 4) { handle(make(Type::Browse, true, (int16_t)(s_browseAcc / 4)), false); s_browseAcc %= 4; }
        } else {
            s_deck->post(make(Type::Jog, false, (int16_t)s.encoder));
        }
    }
    if (s.released[Key] && !s_keyUsed) s_deck->post(make(Type::KeyLock));
    if (s.pressed[Push] || s.pressed[List]) {
        if (s_browsing && s.pressed[Push]) loadSelected(s_selected);
        else if (s_browsing) s_browsing = false;
        else handle(make(Type::Browse, true, 0), false);
    }
    if (s.pressed[Play] || s.released[Play]) s_deck->post(make(Type::PlayPause, s.pressed[Play]));
    if (s.pressed[Cue] || s.released[Cue]) s_deck->post(make(Type::Cue, s.pressed[Cue]));
    if (s.pot >= 0) { s_tempo = s.pot; s_deck->post(make(Type::TempoFader, false, 0, s.pot / 1000.0f)); }
}

static void setTempo(int fader)
{
    s_tempo = fader < 0 ? 0 : fader > 1000 ? 1000 : fader;
    s_deck->post(make(Type::TempoFader, false, 0, s_tempo / 1000.0f));
}

static void consoleBegin()
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t c = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&c) == ESP_OK) usb_serial_jtag_vfs_use_driver();
#endif
}

static void console()
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    uint8_t buf[16];
    const int n = usb_serial_jtag_read_bytes(buf, sizeof buf, 0);
    static uint8_t pkt[4];
    static int pktN = -1;                          // -1: not in a bridge packet
    const nano::DeckStatus st = s_deck->status();
    const int perPct = 1000 / (2 * (st.rangePct ? st.rangePct : 8));   // fader units per 1 %
    for (int i = 0; i < n; i++) {
        if (pktN >= 0) {                           // inside 0x01 + 4 bytes
            pkt[pktN++] = buf[i];
            if (pktN == 4) {
                pktN = -1;
                deck::Event ev[2];
                const int k = s_dj.decodeUsbPacket(pkt, ev, 2);
                for (int j = 0; j < k; j++) handle(ev[j], false);
            }
            continue;
        }
        switch (buf[i]) {
        case 0x01: pktN = 0; break;
        case 'p': s_deck->post(make(Type::PlayPause)); s_deck->post(make(Type::PlayPause, false)); break;
        case 'c': s_deck->post(make(Type::Cue)); s_deck->post(make(Type::Cue, false)); break;
        case 'k': s_deck->post(make(Type::KeyLock)); break;
        case '+': setTempo(s_tempo + perPct); break;
        case '-': setTempo(s_tempo - perPct); break;
        case '0': setTempo(500); break;
        case 'r': s_deck->post(make(Type::TempoRange)); break;
        case 'n': loadSelected((st.track + 1) % (s_lib.count() ? s_lib.count() : 1)); break;
        case 'b': loadSelected(st.track > 0 ? st.track - 1 : 0); break;
        case 'j': s_deck->post(make(Type::Jog, false, -8)); break;
        case 'J': s_deck->post(make(Type::Jog, false, 8)); break;
        case 'l': s_browsing = !s_browsing; break;
        case '?': ESP_LOGI(TAG, "keys: p play, c cue, k keylock, +/- tempo, 0 reset, r range, n/b track, j/J jog, l list"); break;
        default: break;
        }
    }
#endif
}

static const char* nameOf(int i, void*) { nano::Track t; return s_lib.get(i, t) ? t.name : ""; }

static void audioTask(void*)
{
    static int16_t buf[nano::Deck::kBlock * 2];
    for (;;) {
        audio::setRate(s_deck->rate());
        const int64_t t0 = esp_timer_get_time();
        s_deck->render(buf, nano::Deck::kBlock);
        s_busyUs += esp_timer_get_time() - t0;
        s_audioFrames += nano::Deck::kBlock;
        audio::write(buf, nano::Deck::kBlock);
    }
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "freedj-nano on %s", board::NAME);
    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = I2C_NUM_0; bc.sda_io_num = board::I2C_SDA; bc.scl_io_num = board::I2C_SCL;
    bc.clk_source = I2C_CLK_SRC_DEFAULT; bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) bus = nullptr;
    const bool screen = display::begin(bus);

    tracks::begin(s_lib);
    s_deck = new nano::Deck(s_lib);
    if (!audio::begin(44100)) ESP_LOGE(TAG, "I2S failed");
    inputs::begin();
    midi_uart::begin();
    consoleBegin();
    if (s_lib.count()) s_deck->post(make(Type::Load, true, 0));
    xTaskCreate(audioTask, "audio", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr);

    ui::Touch touch;
    ui::Model m;
    m.nameOf = nameOf;
    const int uiPeriodMs = display::width() >= 240 ? 33 : 50;
    int64_t nextUi = 0, nextStat = esp_timer_get_time() + 2000000, uiUs = 0;
    int uiFrames = 0;
    for (;;) {
        controls();
        console();
        midi_uart::poll(onMidi);
        bool down; int x = 0, y = 0;
        if (display::touch(down, x, y)) {
            deck::Event ev[4];
            const int n = touch.feed(m, display::width(), display::height(), down, x, y, ev, 4);
            for (int i = 0; i < n; i++) handle(ev[i], true);
        }

        const int64_t now = esp_timer_get_time();
        if (screen && now >= nextUi) {
            nextUi = now + uiPeriodMs * 1000;
            m.st = s_deck->status();
            if (m.st.track != s_trackIdx) { s_trackIdx = m.st.track; s_lib.get(s_trackIdx, s_track); }
            m.track = s_trackIdx >= 0 ? &s_track : nullptr;
            m.tracks = s_lib.count(); m.selected = s_selected; m.browsing = s_browsing;
            display::present(m);
            uiUs += esp_timer_get_time() - now; uiFrames++;
        }
        if (now >= nextStat) {   // budget line, as freedj-esp's every-2-s report
            nextStat = now + 2000000;
            const nano::DeckStatus st = s_deck->status();
            const int64_t frames = s_audioFrames, busy = s_busyUs;
            s_audioFrames = 0; s_busyUs = 0;
            const int64_t audioUs = st.rate ? frames * 1000000 / st.rate : 0;
            ESP_LOGI(TAG, "%s t%d %.1f s %+.1f %% | audio %.1f %% of the core (%s), ui %.1f ms x %d, free heap %u (min %u)",
                     st.playing ? "PLAY" : st.cueing ? "CUE" : "stop", st.track + 1,
                     st.rate ? (double)(st.posQ16 >> 16) / st.rate : 0.0, st.pitchPermille / 10.0,
                     audioUs ? 100.0 * busy / audioUs : 0.0, st.stretching ? "keylock" : "varispeed",
                     uiFrames ? uiUs / 1000.0 / uiFrames : 0.0, uiFrames,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            uiUs = 0; uiFrames = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
