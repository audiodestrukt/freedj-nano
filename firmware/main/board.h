// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Pin maps. Not checked on hardware yet: confirm against the board's pinout
// before wiring. C6 pins to avoid: 8, 9, 15 (strapping; 9 is BOOT, fine as an
// input after boot), 12/13 (USB), 24-30 (flash).

#include "driver/gpio.h"
#include "sdkconfig.h"

namespace board {

constexpr gpio_num_t NC = GPIO_NUM_NC;

#if CONFIG_NANO_BOARD_XIAO_C6
// Seeed XIAO ESP32C6: D0=0 D1=1 D2=2 D3=21 D4=22 D5=23 D6=16 D7=17 D8=19 D9=20 D10=18.
// Eleven pins, so the four buttons share one ADC pin (resistor ladder) and the
// screen is an I2C OLED. A TFT doesn't fit: use the DevKit.
constexpr const char* NAME = "XIAO ESP32C6";
constexpr gpio_num_t I2S_BCK = GPIO_NUM_1,  I2S_WS = GPIO_NUM_2, I2S_DOUT = GPIO_NUM_18;   // D1, D2, D10 -> PCM5102A
constexpr gpio_num_t I2C_SDA = GPIO_NUM_22, I2C_SCL = GPIO_NUM_23;                         // D4, D5: OLED
constexpr gpio_num_t ENC_A = GPIO_NUM_19,   ENC_B = GPIO_NUM_20, ENC_PUSH = GPIO_NUM_21;   // D8, D9, D3: EC11
constexpr gpio_num_t MIDI_RX = GPIO_NUM_17;                                                 // D7
// Button ladder on D0 (ADC1 ch 0): 10k pull-up to 3V3, each button to GND
// through its resistor. Nominal mV with nothing pressed: 3300.
constexpr gpio_num_t LADDER = GPIO_NUM_0;
constexpr int LADDER_CH = 0;
// PLAY 0R (0 mV), CUE 2k2 (595), KEY 4k7 (1055), LIST 10k (1650)
constexpr int LADDER_MV[4] = {0, 595, 1055, 1650};
constexpr gpio_num_t BTN_PLAY = NC, BTN_CUE = NC, BTN_KEY = NC, BTN_LIST = NC;
constexpr gpio_num_t POT = NC; constexpr int POT_CH = -1;
constexpr gpio_num_t TFT_SCK = NC, TFT_MOSI = NC, TFT_CS = NC, TFT_DC = NC, TFT_RST = NC;

#elif CONFIG_NANO_BOARD_DEVKIT_C6
// ESP32-C6-DevKitC-1. TFT on the SPI2 IOMUX pins (6, 7) for the full clock.
constexpr const char* NAME = "ESP32-C6-DevKitC-1";
constexpr gpio_num_t I2S_BCK = GPIO_NUM_10, I2S_WS = GPIO_NUM_11, I2S_DOUT = GPIO_NUM_21;
constexpr gpio_num_t I2C_SDA = GPIO_NUM_22, I2C_SCL = GPIO_NUM_23;                         // OLED or touch
constexpr gpio_num_t ENC_A = GPIO_NUM_3,    ENC_B = GPIO_NUM_5,  ENC_PUSH = GPIO_NUM_9;    // push = BOOT button too
constexpr gpio_num_t MIDI_RX = GPIO_NUM_20;
constexpr gpio_num_t BTN_PLAY = GPIO_NUM_0, BTN_CUE = GPIO_NUM_1, BTN_KEY = GPIO_NUM_2, BTN_LIST = NC;
constexpr gpio_num_t LADDER = NC; constexpr int LADDER_CH = -1;
constexpr int LADDER_MV[4] = {0, 0, 0, 0};
constexpr gpio_num_t POT = GPIO_NUM_4; constexpr int POT_CH = 4;                           // ADC1 ch 4
constexpr gpio_num_t TFT_SCK = GPIO_NUM_6, TFT_MOSI = GPIO_NUM_7, TFT_CS = GPIO_NUM_18, TFT_DC = GPIO_NUM_19;
constexpr gpio_num_t TFT_RST = NC;   // tie the panel's RST to 3V3: the driver sends a software reset
#endif

} // namespace board
