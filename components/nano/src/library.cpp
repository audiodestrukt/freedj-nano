// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nano/library.h"

#include <string.h>

namespace nano {

static constexpr size_t kHeader = 8, kEntry = 64;

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

bool Library::open(const void* base, size_t size)
{
    base_ = (const uint8_t*)base; size_ = size; count_ = 0;
    if (!base_ || size < kHeader || memcmp(base_, "FDJN", 4)) return false;
    version_ = le16(base_ + 4);
    if (version_ != 1 && version_ != 2) return false;
    const int n = le16(base_ + 6);
    if (kHeader + (size_t)n * kEntry > size) return false;
    count_ = n;
    return true;
}

bool Library::get(int i, Track& out) const
{
    if (i < 0 || i >= count_) return false;
    const uint8_t* e = base_ + kHeader + (size_t)i * kEntry;
    const uint32_t off = le32(e + 40), frames = le32(e + 44), rate = le32(e + 48);
    const uint8_t ch = e[52];
    const bool bands = version_ >= 2 && e[53] == 1;
    const uint32_t poff = le32(e + 56), plen = le32(e + 60);
    if (e[version_ >= 2 ? 31 : 39] != 0 || (ch != 1 && ch != 2) || off % 4) return false;
    if ((uint64_t)off + (uint64_t)frames * ch * 2 > size_ || (uint64_t)poff + (uint64_t)plen * (bands ? 3 : 1) > size_) return false;
    out.name = (const char*)e;
    out.pcm.data = (const int16_t*)(base_ + off);
    out.pcm.frames = frames;
    out.pcm.rate = rate;
    out.pcm.channels = ch;
    out.peaks = plen ? base_ + poff : nullptr;
    out.peaksLen = plen;
    out.peakBucket = plen ? (frames + plen - 1) / plen : 1;
    out.bands = bands;
    out.bpmX100 = version_ >= 2 ? le32(e + 32) : 0;
    out.firstBeat = version_ >= 2 ? le32(e + 36) : 0;
    return true;
}

} // namespace nano
