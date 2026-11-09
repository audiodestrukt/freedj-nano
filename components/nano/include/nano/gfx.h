// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The one drawing interface the UI uses, so the same layout code runs on a
// 128x32 OLED, a 320x240 TFT and the host (screenshots in tests).
//
// Backends: the u8g2 one for mono panels (firmware/main/display_u8g2.cpp), the
// TFT one (display_tft.cpp, renders the screen in bands through a Frame), and
// Frame itself on the host. Colours are RGB565; a mono backend lights any
// non-zero colour. Text is a built-in 5x7 font (6x8 cell) scaled by an integer,
// identical on every backend.

#include <stdint.h>

namespace gfx {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3); }
constexpr uint16_t kBlack = 0, kWhite = 0xFFFF;

class Surface {
public:
    virtual ~Surface() = default;
    virtual int  width() const = 0;
    virtual int  height() const = 0;
    virtual bool mono() const = 0;
    virtual void fill(int x, int y, int w, int h, uint16_t c) = 0;   // clipped by the backend
    // Do rows [y, y + h) land in what's being drawn? A banded backend says no for rows
    // outside the current band, so the UI skips work there (the layout runs once per band).
    virtual bool rows(int y, int h) const { (void)y; (void)h; return true; }
};

void hline(Surface& s, int x, int y, int w, uint16_t c);
void vline(Surface& s, int x, int y, int h, uint16_t c);
void frame(Surface& s, int x, int y, int w, int h, uint16_t c);      // outline
int  text(Surface& s, int x, int y, const char* str, uint16_t c, int scale = 1);   // returns the x after it
inline int textWidth(const char* str, int scale = 1) { int n = 0; while (str[n]) n++; return n * 6 * scale; }

// RGB565 pixels in RAM covering rows [y0, y0 + rows) of a w x h screen: the
// whole screen on the host, one band of it on a TFT. Pixels are stored
// big-endian when `swap` is set (what SPI panels take).
class Frame : public Surface {
public:
    Frame(uint16_t* px, int w, int h, int rows, bool mono = false, bool swap = false)
        : px_(px), w_(w), h_(h), rows_(rows), mono_(mono), swap_(swap) {}
    void setBand(int y0) { y0_ = y0; }
    int  band() const { return y0_; }
    uint16_t* pixels() const { return px_; }
    int width() const override { return w_; }
    int height() const override { return h_; }
    bool mono() const override { return mono_; }
    void fill(int x, int y, int w, int h, uint16_t c) override;
    bool rows(int y, int h) const override { return y < y0_ + rows_ && y + h > y0_; }
private:
    uint16_t* px_;
    int w_, h_, rows_, y0_ = 0;
    bool mono_, swap_;
};

} // namespace gfx
