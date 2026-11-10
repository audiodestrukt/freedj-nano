// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tracks.h"
#include "pff/diskio.h"
#include "pff/pff.h"

static FATFS    s_fs;
static uint32_t s_base;                   // first sector of TRACKS.BIN
static uint32_t s_size;
static uint16_t s_count;

void trk_read(uint32_t off, uint8_t* dst, uint16_t n)
{
    while (n) {
        const uint16_t o = (uint16_t)(off & 511);
        uint16_t k = 512 - o;
        if (k > n) k = n;
        if (off >= s_size || disk_readp(dst, s_base + (off >> 9), o, k) != RES_OK)
            for (uint16_t i = 0; i < k; i++) dst[i] = 0xFF;          // mu-law silence
        off += k; dst += k; n -= k;
    }
}

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int trk_mount(void)
{
    s_count = 0;
    if (disk_initialize()) return TRK_NO_CARD;
    if (pf_mount(&s_fs)) return TRK_NO_FAT;
    if (pf_open("TRACKS.BIN")) return TRK_NO_FILE;
    switch (pf_contig(&s_base)) {
    case FR_OK: break;
    case FR_NO_FILE: return TRK_FRAGMENTED;
    default: return TRK_NO_FAT;
    }
    s_size = s_fs.fsize;
    uint8_t h[8];
    trk_read(0, h, 8);
    if (h[0] != 'F' || h[1] != 'D' || h[2] != 'J' || h[3] != 'L' || h[4] != 1 || h[5]) return TRK_BAD_PACK;
    s_count = (uint16_t)(h[6] | h[7] << 8);
    if (16 + 128 * (uint32_t)s_count > s_size) { s_count = 0; return TRK_BAD_PACK; }
    return TRK_OK;
}

uint16_t trk_count(void) { return s_count; }

static uint32_t entry(uint16_t i) { return 16 + ((uint32_t)i << 7); }

int trk_get(uint16_t i, trk_info* t)
{
    if (i >= s_count) return 0;
    uint8_t e[16];
    trk_read(entry(i) + 32, e, 16);
    t->off = le32(e); t->frames = le32(e + 4); t->bpmX100 = le32(e + 8); t->firstBeat = le32(e + 12);
    if (t->off > s_size || t->frames > s_size - t->off) t->frames = 0;
    return 1;
}

void trk_name(uint16_t i, char* dst)
{
    trk_read(entry(i), (uint8_t*)dst, TRK_NAME - 1);
    dst[TRK_NAME - 1] = 0;
}

void trk_overview(uint16_t i, uint8_t* dst) { trk_read(entry(i) + 48, dst, 64); }
