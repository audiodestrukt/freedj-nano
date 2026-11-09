// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// Mono displays through u8g2 (SSD1306, SH1106, ...): u8g2 owns the controller
// init and the 1 KB frame buffer; our UI draws boxes into it through
// gfx::Surface. Adding another u8g2-supported panel = one more Setup line.
// Also the headless stub when no display is configured.
#include "hw.h"

#include "sdkconfig.h"

#if CONFIG_NANO_DISPLAY_U8G2
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "u8g2.h"

namespace display {

static const char* TAG = "display";
static u8g2_t s_u8g2;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_tx[160];
static size_t s_n;

// u8x8 byte callback over ESP-IDF's I2C master driver: u8g2 sends one
// transfer at a time (command or up to 32 data bytes).
static uint8_t byteCb(u8x8_t*, uint8_t msg, uint8_t n, void* p)
{
    switch (msg) {
    case U8X8_MSG_BYTE_START_TRANSFER: s_n = 0; return 1;
    case U8X8_MSG_BYTE_SEND:
        if (s_n + n > sizeof s_tx) n = (uint8_t)(sizeof s_tx - s_n);
        memcpy(s_tx + s_n, p, n); s_n += n; return 1;
    case U8X8_MSG_BYTE_END_TRANSFER: return i2c_master_transmit(s_dev, s_tx, s_n, 50) == ESP_OK;
    case U8X8_MSG_BYTE_INIT: case U8X8_MSG_BYTE_SET_DC: return 1;
    default: return 0;
    }
}

static uint8_t gpioCb(u8x8_t*, uint8_t msg, uint8_t n, void*)
{
    switch (msg) {
    case U8X8_MSG_DELAY_MILLI: vTaskDelay(pdMS_TO_TICKS(n) ? pdMS_TO_TICKS(n) : 1); break;
    case U8X8_MSG_DELAY_10MICRO: esp_rom_delay_us(10 * n); break;
    case U8X8_MSG_DELAY_100NANO: esp_rom_delay_us(1); break;
    default: break;   // no reset pin on the usual I2C modules
    }
    return 1;
}

class U8g2Surface : public gfx::Surface {
public:
    int width() const override { return u8g2_GetDisplayWidth(&s_u8g2); }
    int height() const override { return u8g2_GetDisplayHeight(&s_u8g2); }
    bool mono() const override { return true; }
    void fill(int x, int y, int w, int h, uint16_t c) override {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width()) w = width() - x;
        if (y + h > height()) h = height() - y;
        if (w <= 0 || h <= 0) return;           // u8g2's coordinates are unsigned
        u8g2_SetDrawColor(&s_u8g2, c ? 1 : 0);
        u8g2_DrawBox(&s_u8g2, x, y, w, h);
    }
};
static U8g2Surface s_surf;

bool begin(i2c_master_bus_handle_t bus)
{
    if (!bus) return false;
    if (i2c_master_probe(bus, 0x3C, 50) != ESP_OK) { ESP_LOGW(TAG, "no OLED at 0x3C"); return false; }
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7; dc.device_address = 0x3C; dc.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) return false;
#if CONFIG_NANO_DISPLAY_SSD1306_128X64
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_u8g2, U8G2_R0, byteCb, gpioCb);
#elif CONFIG_NANO_DISPLAY_SH1106_128X64
    u8g2_Setup_sh1106_i2c_128x64_noname_f(&s_u8g2, U8G2_R0, byteCb, gpioCb);
#elif CONFIG_NANO_DISPLAY_SSD1306_128X32
    u8g2_Setup_ssd1306_i2c_128x32_univision_f(&s_u8g2, U8G2_R0, byteCb, gpioCb);
#endif
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    ESP_LOGI(TAG, "u8g2 %dx%d up", width(), height());
    return true;
}

int width() { return s_surf.width(); }
int height() { return s_surf.height(); }

void present(const ui::Model& m)
{
    u8g2_ClearBuffer(&s_u8g2);
    ui::draw(s_surf, m);
    u8g2_SendBuffer(&s_u8g2);
}

bool touch(bool&, int&, int&) { return false; }

} // namespace display

#elif !CONFIG_NANO_DISPLAY_TFT   // headless

namespace display {
bool begin(i2c_master_bus_handle_t) { return false; }
int width() { return 0; }
int height() { return 0; }
void present(const ui::Model&) {}
bool touch(bool&, int&, int&) { return false; }
}

#endif
