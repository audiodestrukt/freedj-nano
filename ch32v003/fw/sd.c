// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// SD card over SPI for Petit FatFs: disk_initialize() and disk_readp(), after ChaN's
// generic MMC driver. SDv2 (SDHC/SDXC, block addressed) and SDv1 (byte addressed).
//
// Reads come in small pieces (the deck asks for 32 bytes at a time), and a single-block
// read (CMD17) costs a command, the card's access time and all 512 bytes + CRC whatever
// part is wanted. With SD_STREAM (the default) a multi-block read (CMD18) stays open:
// a read just after the last one only clocks the bytes in between, and anything else
// stops the stream (CMD12) and starts a new one. sim/ measures both.
#include "pff/diskio.h"
#include "hal.h"

#ifndef SD_STREAM
#define SD_STREAM 1
#endif
#define SKIP_MAX 2      // sectors: clocking past more than this costs more than a new command

#define CMD0   0
#define CMD8   8
#define CMD12  12
#define CMD16  16
#define CMD17  17
#define CMD18  18
#define CMD55  55
#define CMD58  58
#define ACMD41 (0x80 + 41)

static uint8_t s_block;              // 1: block addressing (SDHC), 0: byte addressing
static uint8_t s_ready;
#if SD_STREAM
static uint8_t  s_open;              // a CMD18 is running
static uint32_t s_sect;              // the sector being clocked out
static uint16_t s_pos;               // bytes of it already clocked (512: at its CRC)
#endif

static void deselect(void) { hal_sd_select(0); hal_spi(0xFF); }

static int waitReady(void)
{
    for (uint16_t n = 0; n < 50000; n++) if (hal_spi(0xFF) == 0xFF) return 1;
    return 0;
}

static uint8_t sendCmd(uint8_t cmd, uint32_t arg)
{
    if (cmd & 0x80) {                                   // ACMD: CMD55 first
        cmd &= 0x7F;
        const uint8_t r = sendCmd(CMD55, 0);
        if (r > 1) return r;
    }
    if (cmd != CMD12) {                                 // CMD12 goes in the middle of a stream
        deselect();
        hal_sd_select(1);
        if (!waitReady()) return 0xFF;
    }
    hal_spi(0x40 | cmd);
    hal_spi((uint8_t)(arg >> 24)); hal_spi((uint8_t)(arg >> 16));
    hal_spi((uint8_t)(arg >> 8));  hal_spi((uint8_t)arg);
    hal_spi(cmd == CMD0 ? 0x95 : cmd == CMD8 ? 0x87 : 0x01);   // CRC only checked for these
    if (cmd == CMD12) hal_spi(0xFF);                    // the stuff byte
    uint8_t r, n = 10;
    do r = hal_spi(0xFF); while ((r & 0x80) && --n);
    return r;
}

// Wait for the data token at the start of a block.
static int token(void)
{
    uint8_t t;
    uint16_t n = 0;
    do t = hal_spi(0xFF); while (t == 0xFF && ++n < 60000);   // ~100 ms at full speed
    return t == 0xFE;
}

static void skip(uint16_t n) { while (n--) hal_spi(0xFF); }

DSTATUS disk_initialize(void)
{
    s_ready = 0;
#if SD_STREAM
    s_open = 0;
#endif
    hal_spi_fast(0);
    hal_sd_select(0);
    for (uint8_t i = 0; i < 10; i++) hal_spi(0xFF);    // 80 clocks with CS high
    uint8_t ty = 0;
    if (sendCmd(CMD0, 0) == 1) {
        const uint32_t t0 = hal_ms();
        if (sendCmd(CMD8, 0x1AA) == 1) {                // SDv2
            uint8_t ocr[4];
            for (uint8_t i = 0; i < 4; i++) ocr[i] = hal_spi(0xFF);
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
                while (sendCmd(ACMD41, 1UL << 30) && hal_ms() - t0 < 1000) {}
                if (hal_ms() - t0 < 1000 && sendCmd(CMD58, 0) == 0) {
                    for (uint8_t i = 0; i < 4; i++) ocr[i] = hal_spi(0xFF);
                    ty = (ocr[0] & 0x40) ? 3 : 2;       // CCS: block addressed
                }
            }
        } else {                                        // SDv1
            while (sendCmd(ACMD41, 0) && hal_ms() - t0 < 1000) {}
            if (hal_ms() - t0 < 1000 && sendCmd(CMD16, 512) == 0) ty = 1;
        }
    }
    deselect();
    if (!ty) return STA_NOINIT;
    s_block = ty == 3;
    s_ready = 1;
    hal_spi_fast(1);
    return 0;
}

#if SD_STREAM

static void stop(void)
{
    if (!s_open) return;
    s_open = 0;
    sendCmd(CMD12, 0);
    waitReady();
    deselect();
}

static int start(uint32_t sector)
{
    stop();
    if (sendCmd(CMD18, s_block ? sector : sector << 9) != 0 || !token()) { deselect(); return 0; }
    s_open = 1; s_sect = sector; s_pos = 0;
    return 1;
}

// Clock forward to (sector, offset); the stream is at or before it.
static int forward(uint32_t sector, uint16_t offset)
{
    while (s_sect != sector) {
        skip(512 - s_pos + 2);                          // rest of the block and its CRC
        if (!token()) { s_open = 0; deselect(); return 0; }
        s_sect++; s_pos = 0;
    }
    skip(offset - s_pos);
    s_pos = offset;
    return 1;
}

DRESULT disk_readp(BYTE* buff, DWORD sector, UINT offset, UINT count)
{
    if (!s_ready) return RES_NOTRDY;
    const int ahead = s_open && (sector > s_sect || (sector == s_sect && offset >= s_pos))
                      && sector - s_sect <= SKIP_MAX;
    if (!ahead && !start(sector)) return RES_ERROR;
    if (!forward(sector, (uint16_t)offset)) return RES_ERROR;
    for (UINT i = 0; i < count; i++) {
        const uint8_t b = hal_spi(0xFF);
        if (buff) buff[i] = b;
    }
    s_pos += (uint16_t)count;
    return RES_OK;
}

#else

DRESULT disk_readp(BYTE* buff, DWORD sector, UINT offset, UINT count)
{
    if (!s_ready) return RES_NOTRDY;
    DRESULT res = RES_ERROR;
    if (sendCmd(CMD17, s_block ? sector : sector << 9) == 0 && token()) {
        skip((uint16_t)offset);
        for (UINT i = 0; i < count; i++) {
            const uint8_t b = hal_spi(0xFF);
            if (buff) buff[i] = b;
        }
        skip((uint16_t)(512 + 2 - offset - count));
        res = RES_OK;
    }
    deselect();
    return res;
}

#endif
