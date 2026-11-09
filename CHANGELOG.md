# freedj-nano changelog

Newest first. Each entry lists what changed, then the budget measured at that point.

## 2026-11-09: one transfer for the waveform

- TFT bands follow the layout's sections (`ui::sections`: header and time / waveform / overview /
  buttons) instead of fixed 48-row strips, so the scrolling waveform reaches the panel in one
  transfer. Split into strips, its bottom strip arrived about a panel refresh after the top and
  showed as a ghost copy at the bottom. Band buffers 2 x 60 KB (were 2 x 30 KB).
- Budget (XIAO, 3.2"): UI 13.0 ms playing (was 18.6: fewer transfers), 28.5 fps; free heap
  290 KB (was 352).

## 2026-11-08 (late): fonts, DJ2Go over the bridge

- The color layouts use proportional fonts: Adobe Helvetica from X11 in u8g2's compressed
  format (10, 12, 14 px and 24 px digits), decoded by our own `gfx::text()` so they go
  through the banded renderer like everything else. `tools/extract_fonts.py` copies them from
  u8g2. The OLED layouts keep the 5x7 font.
- MIDI from freedj-esp's `midi_bridge.py` over the USB console: the DJ2Go plugged into the
  computer drives the XIAO.
- `tools/build.sh` reconfigures before building, so new source files are picked up.
- Budget (XIAO, 3.2"): UI 18.6 ms playing (17.6 before the fonts), 28 fps; image 317 KB (+6 KB
  of fonts); free heap 352 KB.

## 2026-11-08 (night): the 3.2" TFT on the XIAO

- `xiao_tft` config: the XIAO drives the ILI9341 TFT (SCK D8, MOSI D10, CS D3, DC D9, touch on
  D4/D5), the encoder's pins go to the screen and I2S DOUT moves to D6.
- The button ladder pin floated with nothing wired and held CUE at boot: internal pull-up.
- Banded TFT updates: the layout skips anything outside the current band (it ran once per
  band in full: 15x the work), bands 16 -> 48 rows (the waveform goes out in ~3 pieces, not 8:
  less tearing, which showed as ghost copies of the scrolling waveform), SPI 20 -> 40 MHz.
- Budget (XIAO, 3.2" ILI9341V): UI 31 -> 6.5 ms stopped, 45 -> 17.6 ms playing; 28.5 fps (the
  33 ms UI period caps it); free heap 352 KB (two 30 KB band buffers); audio unchanged.

## 2026-11-08 (later): first run on the XIAO ESP32C6

- Serial console over the USB port for the bench (`p`, `c`, `k`, `+`/`-`, `n`/`b`, `j`/`J`, ...).
- First run: boots, maps both tracks (2.4 MB pack), I2S up at 22.05 kHz, plays, cues, changes
  tempo and keylocks. Position tracks tempo (+7.9 %: 2.1 s per 2 s; -7.9 %: 1.8 s). No DAC yet,
  so not heard. No OLED attached: the UI isn't exercised.
- Budget (XIAO, 160 MHz, 22.05 kHz mono): audio stopped 0.5 % of the core, playing 1.4 %,
  keylock 5.0 %; free heap 424 KB, steady. Image 274 KB.

## 2026-11-08: start

- Portable core (`components/nano`): fixed-point keylock (WSOLA reading straight from
  memory-mapped PCM, ~40 bytes of state), varispeed, CDJ-style deck with crossfaded jumps,
  track pack reader, MIDI parser, DJ2Go map and deck events (from freedj-esp), drawing
  interface with a built-in font, UI layouts for 128x32, 128x64 and color TFTs, touch.
- Firmware (`firmware/`, ESP-IDF 5.5, ESP32-C6): I2S to a PCM5102A, EC11 on PCNT, GPIO or
  ladder buttons, pitch pot, DIN MIDI on a UART, u8g2 backend for mono OLEDs, esp_lcd backend
  for the ILI9341 / ST7796 with FT6336 touch and changed-band updates.
- `tools/pack_tracks.py`, `tools/build.sh`, host tests (61 checks).
- Budget: xiao_oled image 269 KB, DIRAM 69 KB of 452 KB; devkit_tft 312 KB, 79 KB. Audio CPU
  not measured (no hardware run yet).
