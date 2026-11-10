// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// An SDHC card in SPI mode, byte by byte as the SPI peripheral sees it: CMD0/8/55/41/58/16,
// single (CMD17) and multi-block (CMD18, stopped by CMD12) reads. The card's access time is
// 0xFF bytes before the data token, so it costs what it would on the bus.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // setup
    int (*read)(void* ctx, uint32_t lba, uint8_t* dst);   // one 512-byte block, 0 = ok
    void*    ctx;
    uint32_t blocks;              // card size in blocks
    uint32_t access, gap;         // busy bytes before a read's first block / between blocks
    uint32_t stallEvery, stall;   // every Nth read command waits `stall` more bytes (0: never)
    uint8_t  initPolls;           // ACMD41s that still answer "idle"
    // state
    uint8_t  sel, idle, app, in[6], nin, resp[8], nresp, iresp, mode, multi, startRead;
    uint32_t wait, lba, nreads;
    uint16_t di;
    uint8_t  blk[512];
    // stats
    uint64_t bytes;               // every byte clocked, selected or not
    uint32_t cmds, reads, blocksOut, stalls, errors;
} sd_emu;

void    sd_emu_reset(sd_emu* c);
void    sd_emu_select(sd_emu* c, int on);
uint8_t sd_emu_xfer(sd_emu* c, uint8_t in);

#ifdef __cplusplus
}
#endif
