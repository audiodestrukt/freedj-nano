// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// hal.h on the PY32F030x8 (Cortex-M0+), registers through Puya's CMSIS header. Bring-up
// stage: clock, time, keys, the SSD1306 on I2C1 and the SD card on SPI1. Not yet: the PWM
// audio (TIM1 + DMA: the blocks are consumed on a timer, silently, so the deck runs), the
// pitch pot (ADC: centred), the USART bridge, and I2C by DMA (sent blocking for now).
//
// Pins (the EmbedFire PY32F030 board: LEDs PA2-PA4, KEY1 PA5, KEY2 PA6, RST):
//   PLAY KEY1 PA5, CUE KEY2 PA6, SHIFT PA7, encoder A / B PA11 / PA12, push PA15 (to ground)
//   OLED SCL PB6, SDA PB7 (I2C1, AF6; checked on the chip: both lines move)
//   AS5600 jog on the same two lines (0x36), DIR to GND; the EC11 still works alongside it
//   SD   SCK PB3, MISO PB4, MOSI PB5 (SPI1, AF0), CS PB0
//   LED2 PA2 on once running; kept free: PA8 (TIM1 CH1, the audio; CH2 needs another pin), PA0 (the pot),
//   (the USART bridge needs other pins)
//
// MEMCARD=1 (make py32 MEMCARD=1): no SD card; the SPI bytes go to the simulator's emulated
// card (sim/emu/sd_emu.c), serving a small card image kept in flash (tools/memcard.py).
#include "py32f030x8.h"
#include "app.h"
#include "hal.h"

#define CLK   48000000u
#define RATE  22059u                          // as the CH32 build: 48 MHz / 544 / 4

// ---- clock: HSI trimmed to 24 MHz (Puya's factory value), PLL x2 = 48 MHz, 1 flash wait
static void clock48(void)
{
    RCC->CR |= RCC_CR_HSION;
    RCC->ICSCR = (RCC->ICSCR & ~(RCC_ICSCR_HSI_FS | RCC_ICSCR_HSI_TRIM))
               | (4u << RCC_ICSCR_HSI_FS_Pos) | (*(const uint32_t*)0x1FFF0F10 & 0x1FFF);
    while (!(RCC->CR & RCC_CR_HSIRDY)) {}
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY;
    RCC->PLLCFGR &= ~RCC_PLLCFGR_PLLSRC;           // from HSI
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_1;       // SYSCLK = PLL, AHB and APB /1
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_1) {}
}

// ---- GPIO helpers
static void pinMode(GPIO_TypeDef* g, int pin, uint32_t mode, uint32_t pull, uint32_t af, int od)
{
    g->MODER = (g->MODER & ~(3u << 2 * pin)) | (mode << 2 * pin);
    g->PUPDR = (g->PUPDR & ~(3u << 2 * pin)) | (pull << 2 * pin);
    g->OTYPER = (g->OTYPER & ~(1u << pin)) | ((uint32_t)od << pin);
    g->OSPEEDR |= 3u << 2 * pin;
    g->AFR[pin >> 3] = (g->AFR[pin >> 3] & ~(15u << 4 * (pin & 7))) | (af << 4 * (pin & 7));
}
enum { IN = 0, OUT = 1, ALT = 2, NOPULL = 0, UP = 1 };

// ---- time: SysTick at 1 kHz; it also plays the DMA's part, consuming audio blocks
static volatile uint32_t s_ms;
static volatile uint8_t  s_rd, s_filled;
static uint32_t s_acc;
static uint8_t  s_ring[HAL_AUDIO_BLOCKS][2 * HAL_AUDIO_BLOCK];

void SysTick_Handler(void)
{
    s_ms++;
    s_acc += RATE;                                // samples * 1000 played this ms
    while (s_acc >= HAL_AUDIO_BLOCK * 1000u) {
        s_acc -= HAL_AUDIO_BLOCK * 1000u;
        if (s_filled) { s_rd = (uint8_t)((s_rd + 1) % HAL_AUDIO_BLOCKS); s_filled--; }
    }
}
uint32_t hal_ms(void) { return s_ms; }

// ---- inputs, to ground with pull-ups
uint8_t hal_pins(void)
{
    const uint32_t a = ~GPIOA->IDR;
    return (uint8_t)(((a >> 11) & 1 ? PIN_ENC_A : 0) | ((a >> 12) & 1 ? PIN_ENC_B : 0) |
                     ((a >> 15) & 1 ? PIN_PUSH : 0) | ((a >> 5) & 1 ? PIN_PLAY : 0) |
                     ((a >> 6) & 1 ? PIN_CUE : 0) | ((a >> 7) & 1 ? PIN_SHIFT : 0));
}
uint16_t hal_pot(void) { return 512; }            // no pot yet: pitch centred
int hal_uart_rx(void) { return -1; }              // no bridge yet

// ---- SD card on SPI1, mode 0, polled; or the card in flash
#if MEMCARD
#include "sd_emu.h"
extern const uint32_t memcard_blocks, memcard_lba[];
extern const uint16_t memcard_count;
extern const uint8_t memcard_data[][512];
static sd_emu s_card;
static int memRead(void* ctx, uint32_t lba, uint8_t* dst)
{
    (void)ctx;
    uint16_t lo = 0, hi = memcard_count;
    while (lo < hi) { const uint16_t m = (uint16_t)((lo + hi) / 2); if (memcard_lba[m] < lba) lo = m + 1; else hi = m; }
    const uint8_t* src = lo < memcard_count && memcard_lba[lo] == lba ? memcard_data[lo] : 0;
    for (int i = 0; i < 512; i++) dst[i] = src ? src[i] : 0;
    return 0;
}
void hal_sd_select(int on) { sd_emu_select(&s_card, on); }
uint8_t hal_spi(uint8_t out) { return sd_emu_xfer(&s_card, out); }
void hal_spi_fast(int on) { (void)on; }
static void sdInit(void)
{
    s_card.read = memRead;
    s_card.blocks = memcard_blocks;
    sd_emu_reset(&s_card);
    s_card.access = 2; s_card.gap = 1;            // flash answers at once
}
#else
void hal_sd_select(int on) { GPIOB->BSRR = on ? (1u << 16) : 1u; }     // CS PB0, low = on
uint8_t hal_spi(uint8_t out)
{
    while (!(SPI1->SR & SPI_SR_TXE)) {}
    *(volatile uint8_t*)&SPI1->DR = out;
    while (!(SPI1->SR & SPI_SR_RXNE)) {}
    return *(volatile uint8_t*)&SPI1->DR;
}
void hal_spi_fast(int on)
{
    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 = (SPI1->CR1 & ~SPI_CR1_BR) | ((on ? 0u : 6u) << SPI_CR1_BR_Pos);   // /2 = 24 MHz, /128 = 375 kHz
    SPI1->CR1 |= SPI_CR1_SPE;
}
#endif

// ---- SSD1306 on I2C1, 400 kHz. Blocking (the DMA version comes with the audio); gives up
// on a missing ACK or a stuck bus rather than hanging, so the deck runs with no screen.
static int waitSR1(uint32_t bit)
{
    for (uint32_t n = 0; n < 20000; n++) {
        const uint32_t sr = I2C1->SR1;
        if (sr & I2C_SR1_AF) { I2C1->SR1 = ~I2C_SR1_AF; return 0; }
        if (sr & bit) return 1;
    }
    return 0;
}
void hal_i2c_send(uint8_t addr, const uint8_t* p, uint16_t n)
{
    I2C1->CR1 |= I2C_CR1_START;
    if (!waitSR1(I2C_SR1_SB)) goto stop;
    I2C1->DR = (uint32_t)addr << 1;
    if (!waitSR1(I2C_SR1_ADDR)) goto stop;
    (void)I2C1->SR2;                              // clears ADDR
    while (n--) {
        if (!waitSR1(I2C_SR1_TXE)) goto stop;
        I2C1->DR = *p++;
    }
    waitSR1(I2C_SR1_BTF);
stop:
    I2C1->CR1 |= I2C_CR1_STOP;
}
int hal_i2c_busy(void) { return 0; }

// Read n (>= 2) bytes from register reg: write it, repeated start, read (the F1-style
// sequence: the last two bytes are NACKed and stopped while BTF holds the bus). 0 on a NACK.
static int i2cRead(uint8_t addr, uint8_t reg, uint8_t* p, uint8_t n)
{
    I2C1->CR1 |= I2C_CR1_START;
    if (!waitSR1(I2C_SR1_SB)) goto fail;
    I2C1->DR = (uint32_t)addr << 1;
    if (!waitSR1(I2C_SR1_ADDR)) goto fail;
    (void)I2C1->SR2;
    I2C1->DR = reg;
    if (!waitSR1(I2C_SR1_BTF)) goto fail;
    I2C1->CR1 |= I2C_CR1_START | I2C_CR1_ACK;
    if (!waitSR1(I2C_SR1_SB)) goto fail;
    I2C1->DR = (uint32_t)addr << 1 | 1;
    if (n == 2) I2C1->CR1 |= I2C_CR1_POS;
    if (!waitSR1(I2C_SR1_ADDR)) goto fail;
    if (n == 2) {
        I2C1->CR1 &= ~I2C_CR1_ACK;
        (void)I2C1->SR2;
        if (!waitSR1(I2C_SR1_BTF)) goto fail;
        I2C1->CR1 |= I2C_CR1_STOP;
        p[0] = (uint8_t)I2C1->DR; p[1] = (uint8_t)I2C1->DR;
        I2C1->CR1 &= ~I2C_CR1_POS;
        return 1;
    }
    (void)I2C1->SR2;
    for (; n > 3; n--) { if (!waitSR1(I2C_SR1_RXNE)) goto fail; *p++ = (uint8_t)I2C1->DR; }
    if (!waitSR1(I2C_SR1_BTF)) goto fail;         // n-2 in DR, n-1 in the shift register
    I2C1->CR1 &= ~I2C_CR1_ACK;
    *p++ = (uint8_t)I2C1->DR;
    I2C1->CR1 |= I2C_CR1_STOP;
    *p++ = (uint8_t)I2C1->DR;
    if (!waitSR1(I2C_SR1_RXNE)) return 0;
    *p = (uint8_t)I2C1->DR;
    return 1;
fail:
    I2C1->CR1 = (I2C1->CR1 & ~(I2C_CR1_ACK | I2C_CR1_POS)) | I2C_CR1_STOP;
    return 0;
}

// ---- the jog: an AS5600 on the OLED's bus (0x36), read every 2 ms. 4096 steps a turn,
// JOG_DIV of them to a count; the remainder carries, so a step's jitter never makes a count.
#define AS5600   0x36
#ifndef JOG_DIV
#define JOG_DIV  32                               // 128 counts a turn (an EC11: 80)
#endif
#ifndef JOG_DIR
#define JOG_DIR  1                                // -1 to turn it the other way (or DIR to 3V3)
#endif
volatile struct { uint16_t raw; uint8_t status, ok; } g_jog;   // to watch it over SWD
static int16_t  s_jogAcc;
static uint16_t s_jogLast = 0xFFFF;
static uint32_t s_jogMs;
int16_t hal_jog(void)
{
    if (hal_ms() - s_jogMs < 2) return 0;
    s_jogMs = hal_ms();
    uint8_t b[3] = { 0 };                         // STATUS, RAW ANGLE high, low
    g_jog.ok = (uint8_t)i2cRead(AS5600, 0x0B, b, 3);
    if (!g_jog.ok) { s_jogMs += 500; return 0; }  // none on the bus: look again in half a second
    const uint16_t a = (uint16_t)((b[1] & 15) << 8 | b[2]);
    g_jog.status = b[0]; g_jog.raw = a;
    if (!(b[0] & 0x20)) { s_jogLast = 0xFFFF; return 0; }       // MD: no magnet
    if (s_jogLast == 0xFFFF) { s_jogLast = a; return 0; }
    const int16_t d = (int16_t)(((a - s_jogLast + 2048) & 4095) - 2048);
    s_jogLast = a;
    s_jogAcc += JOG_DIR * d;
    const int16_t c = s_jogAcc / JOG_DIV;         // toward zero: the remainder keeps its sign
    s_jogAcc -= c * JOG_DIV;
    return c;
}

// ---- audio: blocks go to the ring; SysTick consumes them at the sample rate (no output yet)
uint8_t* hal_audio_block(void) { return s_filled < HAL_AUDIO_BLOCKS ? s_ring[(s_rd + s_filled) % HAL_AUDIO_BLOCKS] : 0; }
void hal_audio_commit(void) { __disable_irq(); s_filled++; __enable_irq(); }

int main(void)
{
    clock48();
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN | RCC_IOPENR_GPIOBEN | RCC_IOPENR_GPIOFEN;
    RCC->APBENR1 |= RCC_APBENR1_I2CEN;
    RCC->APBENR2 |= RCC_APBENR2_SPI1EN;

    static const uint8_t kKeys[] = { 5, 6, 7, 11, 12, 15 };
    for (unsigned i = 0; i < sizeof kKeys; i++) pinMode(GPIOA, kKeys[i], IN, UP, 0, 0);
    for (int pin = 2; pin <= 4; pin++) pinMode(GPIOA, pin, OUT, NOPULL, 0, 0);
    GPIOA->BSRR = 1u << 2;                        // LED2: the firmware is running

    pinMode(GPIOB, 6, ALT, UP, 6, 1);             // SCL
    pinMode(GPIOB, 7, ALT, UP, 6, 1);             // SDA
    RCC->APBRSTR1 |= RCC_APBRSTR1_I2CRST; RCC->APBRSTR1 &= ~RCC_APBRSTR1_I2CRST;
    I2C1->CR2 = CLK / 1000000u;                   // FREQ in MHz
    I2C1->CCR = I2C_CCR_FS | I2C_CCR_DUTY | 5;    // 48 MHz / (25 x 5) = 384 kHz
    I2C1->TRISE = CLK / 1000000u * 300 / 1000 + 1;
    I2C1->CR1 = I2C_CR1_PE;

#if MEMCARD
    sdInit();
#else
    pinMode(GPIOB, 0, OUT, UP, 0, 0); GPIOB->BSRR = 1u;     // CS high
    pinMode(GPIOB, 3, ALT, NOPULL, 0, 0);
    pinMode(GPIOB, 4, ALT, UP, 0, 0);
    pinMode(GPIOB, 5, ALT, NOPULL, 0, 0);
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (6u << SPI_CR1_BR_Pos);
    SPI1->CR2 = SPI_CR2_FRXTH;                    // 8-bit frames, RXNE at one byte
    SPI1->CR1 |= SPI_CR1_SPE;
#endif

    SysTick->LOAD = CLK / 1000u - 1;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;

    app_init();
    for (;;) app_step();
}
