// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The track pack: every track as raw PCM plus a waveform, in one image that
// goes into the `tracks` flash partition (tools/pack_tracks.py writes it).
// Memory-mapped on the target, so loading a track is pointer arithmetic.
//
//   header   "FDJN", u16 version (1), u16 count
//   entries  count x 64 bytes, little-endian:
//              char name[40]      NUL-terminated
//              u32  pcm_offset    from the start of the pack, 4-byte aligned
//              u32  frames
//              u32  rate
//              u8   channels      1 or 2
//              u8   reserved[3]
//              u32  peaks_offset  one byte per bucket: max |sample| >> 7 (0..255)
//              u32  peaks_len
//   data     s16le interleaved PCM and peak tables

#include <stddef.h>
#include <stdint.h>
#include "nano/pcm.h"

namespace nano {

struct Track {
    const char*    name = "";
    Pcm            pcm;
    const uint8_t* peaks = nullptr;
    uint32_t       peaksLen = 0;
    uint32_t       peakBucket = 1;   // frames per peak
};

class Library {
public:
    // Returns false (and count() == 0) if the image isn't a valid pack.
    bool open(const void* base, size_t size);
    int  count() const { return count_; }
    bool get(int i, Track& out) const;

private:
    const uint8_t* base_ = nullptr;
    size_t size_ = 0;
    int count_ = 0;
};

} // namespace nano
