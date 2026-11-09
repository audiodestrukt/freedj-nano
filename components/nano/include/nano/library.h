// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The track pack: every track as raw PCM plus a waveform, in one image that
// goes into the `tracks` flash partition (tools/pack_tracks.py writes it).
// Memory-mapped on the target, so loading a track is pointer arithmetic.
//
//   header   "FDJN", u16 version (1 or 2), u16 count
//   entries  count x 64 bytes, little-endian:
//              v1: char name[40]       NUL-terminated
//              v2: char name[32]; u32 bpm_x100 (0 = unknown); u32 first_beat (frame)
//              u32  pcm_offset    from the start of the pack, 4-byte aligned
//              u32  frames
//              u32  rate
//              u8   channels      1 or 2
//              u8   peak_kind     0: one byte per bucket (max |sample| >> 7);
//                                 1 (v2): three, low / mid / high band (for the CDJ-style colours)
//              u8   reserved[2]
//              u32  peaks_offset
//              u32  peaks_len     buckets (10 ms each)
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
    bool           bands = false;    // peaks are 3 bytes per bucket: low, mid, high
    uint32_t       bpmX100 = 0;      // 0: no beat grid
    uint32_t       firstBeat = 0;    // frame of the first downbeat
    // One peak (0..255) at bucket b: the loudest band when the track has bands.
    uint8_t peak(uint32_t b) const {
        if (!bands) return peaks[b];
        const uint8_t* p = peaks + 3 * b;
        return p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
    }
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
    int version_ = 1;
};

} // namespace nano
