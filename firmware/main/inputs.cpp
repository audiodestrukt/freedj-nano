// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw.h"

#include <stdlib.h>
#include "board.h"
#include "driver/pulse_cnt.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

namespace inputs {

static const char* TAG = "inputs";
static pcnt_unit_handle_t s_enc;
static int s_last = 0;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_raw[kButtons], s_stable[kButtons];
static uint8_t s_count[kButtons];
static int s_pot = -1000;
static constexpr uint8_t kDebounce = 4;     // polls (~20 ms)

static void encoderBegin()
{
    if (board::ENC_A == board::NC) return;
    pcnt_unit_config_t uc = {};
    uc.low_limit = -10000; uc.high_limit = 10000;
    ESP_ERROR_CHECK(pcnt_new_unit(&uc, &s_enc));
    pcnt_glitch_filter_config_t fc = {};
    fc.max_glitch_ns = 1000;
    pcnt_unit_set_glitch_filter(s_enc, &fc);
    pcnt_chan_config_t a = {}, b = {};
    a.edge_gpio_num = board::ENC_A; a.level_gpio_num = board::ENC_B;
    b.edge_gpio_num = board::ENC_B; b.level_gpio_num = board::ENC_A;
    pcnt_channel_handle_t ca, cb;
    ESP_ERROR_CHECK(pcnt_new_channel(s_enc, &a, &ca));
    ESP_ERROR_CHECK(pcnt_new_channel(s_enc, &b, &cb));
    // full quadrature: 4 counts per EC11 detent
    pcnt_channel_set_edge_action(ca, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(ca, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(cb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(cb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    gpio_pullup_en(board::ENC_A);           // EC11 common to GND
    gpio_pullup_en(board::ENC_B);
    pcnt_unit_enable(s_enc);
    pcnt_unit_clear_count(s_enc);
    pcnt_unit_start(s_enc);
}

void begin()
{
    encoderBegin();
    const gpio_num_t pins[kButtons] = {board::BTN_PLAY, board::BTN_CUE, board::BTN_KEY, board::BTN_LIST, board::ENC_PUSH};
    for (gpio_num_t p : pins) {
        if (p == board::NC) continue;
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << p; c.mode = GPIO_MODE_INPUT; c.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&c);
    }
    if (board::LADDER_CH >= 0 || board::POT_CH >= 0) {
        adc_oneshot_unit_init_cfg_t u = {};
        u.unit_id = ADC_UNIT_1;
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));
        adc_oneshot_chan_cfg_t cc = {};
        cc.atten = ADC_ATTEN_DB_12; cc.bitwidth = ADC_BITWIDTH_12;
        if (board::LADDER_CH >= 0) adc_oneshot_config_channel(s_adc, (adc_channel_t)board::LADDER_CH, &cc);
        if (board::POT_CH >= 0) adc_oneshot_config_channel(s_adc, (adc_channel_t)board::POT_CH, &cc);
        adc_cali_curve_fitting_config_t cf = {};
        cf.unit_id = ADC_UNIT_1; cf.atten = ADC_ATTEN_DB_12; cf.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&cf, &s_cali) != ESP_OK) s_cali = nullptr;
    }
    ESP_LOGI(TAG, "encoder %d/%d, %s buttons", board::ENC_A, board::ENC_B, board::LADDER_CH >= 0 ? "ladder" : "GPIO");
}

static int readMv(int ch)
{
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, (adc_channel_t)ch, &raw) != ESP_OK) return -1;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv;
    return raw * 3300 / 4095;
}

void poll(State& s)
{
    s = State{};
    if (s_enc) {
        int c = 0;
        pcnt_unit_get_count(s_enc, &c);
        s.encoder = c - s_last;
        s_last = c;
        if (abs(c) > 8000) { pcnt_unit_clear_count(s_enc); s_last = 0; }
    }

    bool raw[kButtons] = {};
    const gpio_num_t pins[kButtons] = {board::BTN_PLAY, board::BTN_CUE, board::BTN_KEY, board::BTN_LIST, board::ENC_PUSH};
    for (int i = 0; i < kButtons; i++)
        if (pins[i] != board::NC) raw[i] = gpio_get_level(pins[i]) == 0;
    if (board::LADDER_CH >= 0) {
        // nearest ladder voltage within 250 mV; one button at a time
        const int mv = readMv(board::LADDER_CH);
        for (int i = 0; i < 4; i++)
            if (mv >= 0 && abs(mv - board::LADDER_MV[i]) < 250) raw[i] = true;
    }
    for (int i = 0; i < kButtons; i++) {
        if (raw[i] != s_raw[i]) { s_raw[i] = raw[i]; s_count[i] = 0; }
        else if (s_count[i] < kDebounce && ++s_count[i] == kDebounce && s_stable[i] != raw[i]) {
            s_stable[i] = raw[i];
            (raw[i] ? s.pressed : s.released)[i] = true;
        }
        s.down[i] = s_stable[i];
    }

    if (board::POT_CH >= 0) {
        const int mv = readMv(board::POT_CH);
        const int p = mv < 0 ? -1 : mv * 1000 / 3100 > 1000 ? 1000 : mv * 1000 / 3100;
        if (p >= 0 && abs(p - s_pot) > 3) { s_pot = p; s.pot = p; }   // hysteresis: 0.3 % of travel
    }
}

} // namespace inputs
