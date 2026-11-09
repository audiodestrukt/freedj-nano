// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw.h"

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace audio {

static const char* TAG = "audio";
static i2s_chan_handle_t s_tx;
static uint32_t s_rate;
static volatile uint32_t s_underruns;

// The DMA came round to a buffer we hadn't refilled: it sent silence (auto_clear). A dropout.
static bool IRAM_ATTR onUnderrun(i2s_chan_handle_t, i2s_event_data_t*, void*)
{
    s_underruns = s_underruns + 1;
    return false;
}

bool begin(uint32_t rate)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = 256;      // = one deck block: ~4 blocks (23 ms at 44.1k) of slack
    cc.auto_clear = true;        // underrun plays silence, not the last buffer
    if (i2s_new_channel(&cc, &s_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;   // BCK = 64 fs: the PCM5102A's PLL locks on it without MCLK
    sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    sc.gpio_cfg.bclk = board::I2S_BCK;
    sc.gpio_cfg.ws = board::I2S_WS;
    sc.gpio_cfg.dout = board::I2S_DOUT;
    sc.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK) return false;
    i2s_event_callbacks_t cb = {};
    cb.on_send_q_ovf = onUnderrun;
    i2s_channel_register_event_callback(s_tx, &cb, nullptr);
    if (i2s_channel_enable(s_tx) != ESP_OK) return false;
    s_rate = rate;
    ESP_LOGI(TAG, "I2S up: %u Hz, BCK %d WS %d DOUT %d", (unsigned)rate, board::I2S_BCK, board::I2S_WS, board::I2S_DOUT);
    return true;
}

void setRate(uint32_t rate)
{
    if (!s_tx || !rate || rate == s_rate) return;
    i2s_channel_disable(s_tx);
    i2s_std_clk_config_t c = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_reconfig_std_clock(s_tx, &c);
    i2s_channel_enable(s_tx);
    s_rate = rate;
    ESP_LOGI(TAG, "rate %u Hz", (unsigned)rate);
}

uint32_t underruns() { uint32_t n = s_underruns; s_underruns = 0; return n; }

void write(const int16_t* stereo, int frames)
{
    size_t done = 0;
    i2s_channel_write(s_tx, stereo, (size_t)frames * 4, &done, portMAX_DELAY);
}

} // namespace audio
