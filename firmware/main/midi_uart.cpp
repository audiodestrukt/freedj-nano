// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// DIN/TRS MIDI in on UART1. The C6 has no USB host, so a USB controller like
// the DJ2Go needs a USB-to-DIN adapter (or BLE-MIDI, later). The input needs
// the usual optocoupler (6N138 or H11L1) per the MIDI spec.
#include "hw.h"

#include "board.h"
#include "driver/uart.h"
#include "nano/midi.h"
#include "sdkconfig.h"

namespace midi_uart {

static bool s_up;
static nano::MidiParser s_parser;

void begin()
{
#if CONFIG_NANO_MIDI_UART
    if (board::MIDI_RX == board::NC) return;
    uart_config_t c = {};
    c.baud_rate = 31250; c.data_bits = UART_DATA_8_BITS; c.parity = UART_PARITY_DISABLE;
    c.stop_bits = UART_STOP_BITS_1; c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE; c.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(UART_NUM_1, 512, 0, 0, nullptr, 0) != ESP_OK) return;
    uart_param_config(UART_NUM_1, &c);
    uart_set_pin(UART_NUM_1, UART_PIN_NO_CHANGE, board::MIDI_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_up = true;
#endif
}

void poll(void (*onMsg)(uint8_t, uint8_t, uint8_t))
{
    if (!s_up) return;
    uint8_t buf[64];
    int n;
    while ((n = uart_read_bytes(UART_NUM_1, buf, sizeof buf, 0)) > 0)
        for (int i = 0; i < n; i++) {
            nano::MidiMsg m;
            if (s_parser.feed(buf[i], m)) onMsg(m.status, m.d1, m.d2);
        }
}

} // namespace midi_uart
