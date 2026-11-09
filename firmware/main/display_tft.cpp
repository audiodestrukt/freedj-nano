// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// SPI TFTs (ILI9341 320x240, ST7796 480x320) with FT6336 touch, without LVGL.
// No full frame buffer (a 320x240 frame is 150 KB of the C6's 512 KB): the UI
// is drawn band by band into two DMA buffers of up to 96 rows (60 KB each at 320 wide).
// Bands follow the layout's sections, so the scrolling waveform goes out in one transfer, and a band goes over SPI
// only if its pixels changed since the last frame (hash per band). A playing
// deck mostly changes the waveform and the time, so most bands are skipped.
#include "hw.h"

#include "sdkconfig.h"

#if CONFIG_NANO_DISPLAY_TFT
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_ili9341.h"

namespace display {

static const char* TAG = "display";
#if CONFIG_NANO_DISPLAY_ILI9341
static constexpr int W = 320, H = 240;
#else
static constexpr int W = 480, H = 320;
#endif
static constexpr int kRows = 96;                    // the largest band (the waveform is 94 rows on the 3.2")
static constexpr int kMaxBands = 16;
static int s_band[kMaxBands + 1], s_nBands;         // band edges: the layout's sections, split to <= kRows
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_touch;
static uint16_t* s_buf[2];
static int s_next;
static SemaphoreHandle_t s_free;          // counts band buffers not on the wire
static uint32_t s_hash[H];       // per row: only rows whose pixels changed are sent
static uint32_t s_sentBytes;
static bool s_sendAll;           // diagnostic: send every band every frame (console 'a')

#if CONFIG_NANO_DISPLAY_ST7796
// The ST7796 runs on the ILI9341 driver with its own init sequence (from
// espressif/esp_lcd_st7796, Apache-2.0): that component's manifest leaves out
// the C6, though it's plain SPI. Both drivers send SLPOUT, MADCTL, COLMOD first.
static const ili9341_lcd_init_cmd_t kSt7796Init[] = {
    {0xf0, (const uint8_t[]){0xc3}, 1, 0},
    {0xf0, (const uint8_t[]){0x96}, 1, 0},
    {0xb4, (const uint8_t[]){0x01}, 1, 0},
    {0xb7, (const uint8_t[]){0xc6}, 1, 0},
    {0xe8, (const uint8_t[]){0x40, 0x8a, 0x00, 0x00, 0x29, 0x19, 0xa5, 0x33}, 8, 0},
    {0xc1, (const uint8_t[]){0x06}, 1, 0},
    {0xc2, (const uint8_t[]){0xa7}, 1, 0},
    {0xc5, (const uint8_t[]){0x18}, 1, 0},
    {0xe0, (const uint8_t[]){0xf0, 0x09, 0x0b, 0x06, 0x04, 0x15, 0x2f, 0x54, 0x42, 0x3c, 0x17, 0x14, 0x18, 0x1b}, 14, 0},
    {0xe1, (const uint8_t[]){0xf0, 0x09, 0x0b, 0x06, 0x04, 0x03, 0x2d, 0x43, 0x42, 0x3b, 0x16, 0x14, 0x17, 0x1b}, 14, 0},
    {0xf0, (const uint8_t[]){0x3c}, 1, 0},
    {0xf0, (const uint8_t[]){0x69}, 1, 0},
};
static const ili9341_vendor_config_t kSt7796Vendor = {kSt7796Init, sizeof kSt7796Init / sizeof kSt7796Init[0]};
#endif

static bool IRAM_ATTR onDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_free, &woke);
    return woke == pdTRUE;
}

// The ILI9341's power and VCOM settings. The driver's defaults are for TN panels: on the
// 3.2" IPS module (ILI9341V) they left sharp shadows of the moving waveform in the rows
// below it; the IPS values cleared them (2026-11-09). IPS panels (CONFIG_NANO_TFT_INVERT)
// get them at boot; the console's i / v / V / f keys tune them live.
#if CONFIG_NANO_DISPLAY_ILI9341
struct Preset { const char* name; uint8_t c0, c1, c5a, c5b, c7; };
static const Preset kPresets[] = {
    {"driver default", 0x23, 0x11, 0x43, 0x4C, 0xA0},
    {"Adafruit",       0x23, 0x10, 0x3E, 0x28, 0x86},
    {"IPS",            0x13, 0x13, 0x22, 0x35, 0xBD},
};
static const uint8_t kFrameRates[] = {0x1B, 0x18, 0x13, 0x10};   // B1 RTNA: 70, 79, 100, 119 Hz
static const int kHz[] = {70, 79, 100, 119};
static int s_preset, s_fr;
static int s_vcom = 0xA0;

static void send(uint8_t cmd, const uint8_t* d, int n) { esp_lcd_panel_io_tx_param(s_io, cmd, d, n); }

static void applyPreset(int i)
{
    s_preset = i;
    const Preset& p = kPresets[i];
    send(0xC0, &p.c0, 1); send(0xC1, &p.c1, 1);
    const uint8_t c5[2] = {p.c5a, p.c5b}; send(0xC5, c5, 2);
    s_vcom = p.c7;
    const uint8_t c7 = (uint8_t)s_vcom; send(0xC7, &c7, 1);
}

#endif

static void beginTouch(i2c_master_bus_handle_t bus)
{
#if CONFIG_NANO_TOUCH_FT6336
    if (!bus || i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no touch controller at 0x%02X", ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS);
        return;
    }
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_i2c_config_t ioc = {};
    ioc.dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS;
    ioc.scl_speed_hz = 400000;
    ioc.control_phase_bytes = 1;
    ioc.lcd_cmd_bits = 8;
    ioc.flags.disable_control_phase = 1;
    if (esp_lcd_new_panel_io_i2c(bus, &ioc, &io) != ESP_OK) return;
    esp_lcd_touch_config_t tc = {};
    tc.x_max = H;               // native portrait coordinates, turned to landscape
    tc.y_max = W;
    tc.rst_gpio_num = GPIO_NUM_NC;
    tc.int_gpio_num = GPIO_NUM_NC;
    tc.flags.swap_xy = 1;       // as freedj-esp's display.cpp: verify on the panel
    tc.flags.mirror_y = 1;
    if (esp_lcd_touch_new_i2c_ft5x06(io, &tc, &s_touch) != ESP_OK) s_touch = nullptr;
    else ESP_LOGI(TAG, "touch up");
#else
    (void)bus;
#endif
}

bool begin(i2c_master_bus_handle_t bus)
{
    spi_bus_config_t bc = {};
    bc.sclk_io_num = board::TFT_SCK; bc.mosi_io_num = board::TFT_MOSI;
    bc.miso_io_num = -1; bc.quadwp_io_num = -1; bc.quadhd_io_num = -1;
    bc.max_transfer_sz = W * kRows * 2;
    if (spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    s_free = xSemaphoreCreateCounting(2, 2);
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t ioc = {};
    ioc.cs_gpio_num = board::TFT_CS; ioc.dc_gpio_num = board::TFT_DC;
    ioc.spi_mode = 0; ioc.pclk_hz = CONFIG_NANO_TFT_SPI_MHZ * 1000 * 1000;
    ioc.trans_queue_depth = 4; ioc.lcd_cmd_bits = 8; ioc.lcd_param_bits = 8;
    ioc.on_color_trans_done = onDone;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io) != ESP_OK) return false;
    s_io = io;

    esp_lcd_panel_dev_config_t pc = {};
    pc.reset_gpio_num = board::TFT_RST;
    pc.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    pc.bits_per_pixel = 16;
#if CONFIG_NANO_DISPLAY_ST7796
    pc.vendor_config = (void*)&kSt7796Vendor;
#endif
    if (esp_lcd_new_panel_ili9341(io, &pc, &s_panel) != ESP_OK) return false;
    esp_lcd_panel_reset(s_panel);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_lcd_panel_init(s_panel);
#if CONFIG_NANO_TFT_INVERT
    esp_lcd_panel_invert_color(s_panel, true);
#if CONFIG_NANO_DISPLAY_ILI9341
    applyPreset(2);                                  // IPS voltages
#endif
#endif
    esp_lcd_panel_swap_xy(s_panel, true);           // landscape, header on the left (as freedj-esp)
    esp_lcd_panel_mirror(s_panel, true, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    for (auto& b : s_buf) {
        b = (uint16_t*)heap_caps_malloc(W * kRows * 2, MALLOC_CAP_DMA);
        if (!b) return false;
    }
    memset(s_hash, 0xFF, sizeof s_hash);            // first frame sends everything
    int edges[8];
    const int n = ui::sections(W, H, edges, 8);
    s_nBands = 0; s_band[0] = 0;
    for (int i = 1; i < n; i++)
        for (int y = s_band[s_nBands]; y < edges[i] && s_nBands < kMaxBands; )
            y = s_band[++s_nBands] = edges[i] - y > kRows ? y + kRows : edges[i];
    beginTouch(bus);
    char list[64]; int k = 0;
    for (int i = 0; i <= s_nBands; i++) k += snprintf(list + k, sizeof list - k, "%s%d", i ? "/" : "", s_band[i]);
    ESP_LOGI(TAG, "TFT %dx%d at %d MHz, bands at %s", W, H, CONFIG_NANO_TFT_SPI_MHZ, list);
    return true;
}

int width() { return W; }
int height() { return H; }

void present(const ui::Model& m)
{
    for (int b = 0; b < s_nBands; b++) {
        const int y = s_band[b], rows = s_band[b + 1] - y;
        xSemaphoreTake(s_free, portMAX_DELAY);      // a buffer that's off the wire
        uint16_t* px = s_buf[s_next];
        gfx::Frame f(px, W, H, rows, false, true);
        f.setBand(y);
        ui::draw(f, m);
        // FNV-1a per row; send the span from the first to the last changed row
        int first = -1, last = -1;
        for (int r = 0; r < rows; r++) {
            uint32_t h = 2166136261u;
            const uint32_t* w32 = (const uint32_t*)(px + r * W);
            for (int i = 0; i < W / 2; i++) h = (h ^ w32[i]) * 16777619u;
            if (h != s_hash[y + r] || s_sendAll) { s_hash[y + r] = h; if (first < 0) first = r; last = r; }
        }
        if (first < 0) { xSemaphoreGive(s_free); continue; }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y + first, W, y + last + 1, px + first * W);   // onDone gives the buffer back
        s_sentBytes += (last - first + 1) * W * 2;
        s_next ^= 1;
    }
}

void tune(char key)
{
#if CONFIG_NANO_DISPLAY_ILI9341
    if (!s_io) return;
    if (key == 'R') { memset(s_hash, 0xFF, sizeof s_hash); ESP_LOGI(TAG, "repaint all bands once"); return; }
    if (key == 'a') { s_sendAll = !s_sendAll; ESP_LOGI(TAG, "send every band: %s", s_sendAll ? "on" : "off"); return; }
    if (key == 'i') {
        applyPreset((s_preset + 1) % (int)(sizeof kPresets / sizeof kPresets[0]));
    } else if (key == 'v' || key == 'V') {
        s_vcom += key == 'V' ? 4 : -4;
        s_vcom = s_vcom < 0x80 ? 0x80 : s_vcom > 0xFF ? 0xFF : s_vcom;   // 0x80..0xFF: VCOM offset -63..+63
    } else if (key == 'f') {
        s_fr = (s_fr + 1) % 4;
        const uint8_t b1[2] = {0x00, kFrameRates[s_fr]}; send(0xB1, b1, 2);
    }
    const uint8_t c7 = (uint8_t)s_vcom; send(0xC7, &c7, 1);
    const Preset& p = kPresets[s_preset];
    ESP_LOGI(TAG, "panel: %s (C0 %02X C1 %02X C5 %02X %02X), VCOM2 C7 %02X, %d Hz",
             p.name, p.c0, p.c1, p.c5a, p.c5b, s_vcom, kHz[s_fr]);
#else
    (void)key;
#endif
}

uint32_t sentBytes() { uint32_t b = s_sentBytes; s_sentBytes = 0; return b; }

bool touch(bool& down, int& x, int& y)
{
    if (!s_touch) return false;
    esp_lcd_touch_read_data(s_touch);
    esp_lcd_touch_point_data_t pt[1];
    uint8_t n = 0;
    down = esp_lcd_touch_get_data(s_touch, pt, &n, 1) == ESP_OK && n > 0;
    if (down) { x = pt[0].x; y = pt[0].y; }
    return true;
}

} // namespace display
#endif
