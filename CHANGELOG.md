# freedj-nano changelog

Newest first. Each entry lists what changed, then the budget measured at that point.

## 2026-11-09 (night): rekordbox data

- `tools/rekordbox.py` reads a rekordbox USB export's analysis files (PIONEER/USBANLZ, per
  Deep Symmetry's crate-digger spec): the audio path (PPTH), the beat grid (PQTZ) and the
  CDJ-3000's 3-band scrolling waveform (PWV7 in .2EX, 150 columns per second). PWV7's byte
  order (low, mid, high) isn't in the spec; correlating it against our own band filters on three
  tracks settled it (byte 0 with low +0.68..+0.90, byte 2 with high +0.32..+0.57).
- `pack_tracks.py --rekordbox STICK` uses that grid and waveform for files on the stick (else
  it analyses as before), takes titles from the tags, and `--start` / `--length` cut a section so
  long tracks fit in flash (the grid's first downbeat shifts with it).
- 3-band waveform in freedj-3000's colours and layering (lows outermost, mids, highs innermost).
- Tested with 2AR "Grip" (126 BPM) and MOKAY "MA BOOGIE" (128 BPM), 27 s each from 1:00.

## 2026-11-09 (evening): the CDJ-3000 look

- Color layouts restyled after the CDJ-3000 play screen, in freedj-esp's palette: deck badge
  and title; REMAIN time, TEMPO with an MT (master tempo = keylock) badge, BPM at the current
  tempo; a beat-in-bar meter; the waveform in rekordbox's 3-band colours (lows blue, mids
  orange, highs white; the played side dimmed) with beat ticks and red bar ticks and an orange
  cue marker; the overview in the same colours with the played part greyed; key-style buttons
  (CUE, PLAY/PAUSE, MT, LIST).
- Pack format v2: name 32 bytes, then BPM x 100 and the first downbeat; peaks can be 3 bytes
  per bucket (low < 200 Hz, mid, high > 2 kHz). v1 packs still load. `pack_tracks.py` filters the
  bands and finds the BPM and downbeat (onset autocorrelation; `--bpm` overrides).
- Budget (XIAO, 3.2"): 50 fps playing (was 53), UI 15.6 ms of CPU per frame (was 10.5: three
  bands per column), still SPI-bound at 62 KB per frame; 0 dropouts.

## 2026-11-09 (later): dropout monitor, keylock at 44.1 kHz stereo

- The budget line counts **dropouts** (I2S DMA buffers that went out unfilled: the driver's
  send-queue-overflow event) and the **worst block**: the slowest render of the last 2 s as a
  share of the block's deadline (256 frames: 5.8 ms at 44.1 kHz). Over 100 % is a dropout.
- `pack_tracks.py`: per-file rate and channels (`file@44100s`), so one pack can mix qualities.
- Budget (XIAO, 160 MHz, 3.2" TFT running): **keylock at 44.1 kHz stereo 16.4 % of the core,
  worst block 30-34 % of its deadline, 0 dropouts** (+3.9 % and -7.9 %); varispeed 3.1 %, worst
  4 %. At 22.05 kHz mono keylock is 5.0 %. The UI drops from 53 to ~43 fps while keylock runs at
  44.1 kHz (they share the core; audio has priority).

## 2026-11-09 (later): IPS voltages, 53 fps, fps meter

- **The waveform's shadows were the panel's voltages.** The ILI9341 driver's defaults are for
  TN panels; on the 3.2" IPS module (ILI9341V) they left sharp copies of the moving waveform in
  the rows below it (not power: same on 3V3; not the data path: same with every band re-sent).
  IPS values (C0 13, C1 13, C5 22 35, C7 BD) cleared them; IPS panels (`NANO_TFT_INVERT`) get
  them at boot. Console keys `i` / `v` / `V` / `f` tune power, VCOM and refresh live; `R` and
  `a` are the repaint diagnostics. The values are a common IPS ILI9341V set, from memory: check
  against the module vendor's init code. freedj-esp's A1S drives the same panel on the defaults.
- Only rows that changed are sent (a hash per row, not per band): the header goes out as the
  ~24 rows of the time instead of 60.
- No 33 ms frame cap on the TFT: it runs as fast as SPI allows. fps shown in the header.
- Budget (XIAO, 3.2", 40 MHz): **53 fps** playing (was 28.5), 61 KB per frame (the waveform is
  60 KB: SPI is now the limit), UI 10.5 ms of CPU per frame; keylock 5.0 %; free heap 289 KB.

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
