// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Proportional bitmap fonts in u8g2's compressed format, drawn by gfx::text().
// The data is in fonts.cpp (tools/extract_fonts.py copies it from u8g2).

#include <stdint.h>

namespace gfx {

struct Font { const uint8_t* data; };

extern const Font kHelvR10;      // ~10 px: small labels
extern const Font kHelvR12;      // list rows
extern const Font kHelvR14;      // header, buttons, tempo
extern const Font kHelvR24Num;   // the big time: digits and + , - . / : only

} // namespace gfx
