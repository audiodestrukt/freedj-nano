// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// TRACKS.BIN on the SD card's FAT32 root, written by tools/pack_lite.py:
//
//   header  16 bytes: "FDJL", u16 version (1), u16 count, u32 rate (22050), u32 reserved
//   entries count x 128 bytes at 16 + 128 i, little-endian:
//             char name[32]     NUL-padded
//             u32  data_off     mu-law bytes from here, a multiple of 512
//             u32  frames
//             u32  bpm_x100     0 = unknown
//             u32  first_beat   frame
//             u8   overview[64] 128 columns of 0..15, low nibble first
//             u8   reserved[16]
//   data    mono 22.05 kHz G.711 mu-law, one byte per frame
//
// The file must be in consecutive clusters (it is when written to a freshly formatted
// card): it's then read by sector number, with no FAT lookups after the mount.
#include <stdint.h>

#define TRK_NAME 22                       // characters kept (21 shown, + NUL)

enum { TRK_OK, TRK_NO_CARD, TRK_NO_FAT, TRK_NO_FILE, TRK_FRAGMENTED, TRK_BAD_PACK };

typedef struct { uint32_t off, frames, bpmX100, firstBeat; } trk_info;

int      trk_mount(void);                 // TRK_OK or an error above
uint16_t trk_count(void);
int      trk_get(uint16_t i, trk_info* t);
void     trk_name(uint16_t i, char* dst); // TRK_NAME bytes
void     trk_overview(uint16_t i, uint8_t* dst);   // 64 bytes
void     trk_read(uint32_t off, uint8_t* dst, uint16_t n);   // bytes of TRACKS.BIN
