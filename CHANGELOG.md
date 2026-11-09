# freedj-nano changelog

Newest first. Each entry lists what changed, then the budget measured at that point.

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
