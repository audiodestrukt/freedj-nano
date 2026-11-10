// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// What the deck firmware needs from the chip. ch32fun drivers implement it on the CH32V003
// (phase 1-2); sim/host implements it on the computer with emulated parts (an SD card on
// SPI, an SSD1306 on I2C, an EC11, buttons, a pot, a UART), and sim/qemu for the RV32EC
// instruction count. The firmware is the same in all three.
//
// The main loop is app_step() over and over; nothing blocks for long except SD reads.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- time
uint32_t hal_ms(void);                    // since boot

// ---- inputs: GPIO levels, sampled now. A bit set = contact closed (buttons are to ground).
enum {
    PIN_ENC_A = 1, PIN_ENC_B = 2,         // EC11 quadrature
    PIN_PUSH = 4,                         // EC11 push
    PIN_PLAY = 8, PIN_CUE = 16, PIN_SHIFT = 32,
};
uint8_t  hal_pins(void);
uint16_t hal_pot(void);                   // pitch pot, ADC 0..1023

// ---- USART: the bridge from the computer (DJ2Go as 0x01 + a USB-MIDI packet, console keys)
int      hal_uart_rx(void);               // next byte, or -1

// ---- SD card on SPI (mode 0)
void     hal_sd_select(int on);           // CS low when on
uint8_t  hal_spi(uint8_t out);            // one byte each way
void     hal_spi_fast(int on);            // 0: ~400 kHz for the card's init, 1: full speed

// ---- SSD1306 on I2C, sent by DMA: returns at once; p must stay valid while busy
void     hal_i2c_send(uint8_t addr, const uint8_t* p, uint16_t n);
int      hal_i2c_busy(void);

// ---- audio: TIM1 PWM fed by circular DMA. HAL_AUDIO_BLOCKS blocks of HAL_AUDIO_BLOCK
// samples, each sample two bytes, high then low duty (CH1, CH2).
#define HAL_AUDIO_BLOCK  32
#define HAL_AUDIO_BLOCKS 4
uint8_t* hal_audio_block(void);           // the next free block to fill, or 0 if none is free
void     hal_audio_commit(void);          // that block is filled

#ifdef __cplusplus
}
#endif
