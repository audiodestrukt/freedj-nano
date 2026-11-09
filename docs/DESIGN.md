# freedj-nano: design decisions

How freedj-nano came to be what it is, and why: each decision with the reasoning at the time,
the measurements that settled it, and the wrong turns. Written 2026-11-09, after the first two
days of work. Numbers are from a Seeed XIAO ESP32C6 at 160 MHz unless noted.

## 1. A separate project, not a freedj-esp target

freedj-esp (P4, S3, A1S) assumes **two cores, an FPU and PSRAM**: audio on one core, decoding
and LVGL on the other, tracks decoded into PSRAM. The C6 has one core, no FPU and no PSRAM.
Supporting it in freedj-esp would have meant `#if` branches through the whole audio path
for a target that shares almost none of it.

So freedj-nano is its own repo with its own engine. What both share is copied, not linked:
the deck event vocabulary (`deck_event.h`, with `JogTouch` appended) and the DJ2Go MIDI map.
freedj-esp stops at the S3; anything single-core or smaller lives here.

## 2. The audio engine: fixed point, random access, no buffers

**Tracks are PCM, memory-mapped from flash.** No MP3 decoder and no file system: a
`tracks` partition holds 16-bit PCM plus a 10 ms peak table per track
(`tools/pack_tracks.py`), mapped into the address space with `esp_partition_mmap`. Loading a
track is pointer arithmetic, and every sample is a memory read. That removes the most
expensive thing freedj-esp does (decoding) and makes the whole track random-access.

**Random access shaped the keylock.** freedj-esp's WSOLA pushes decoded audio through input
and overlap-add buffers (~20 KB). With the whole track addressable, nano's WSOLA reads straight
from it. Each output sample is two frames at 50 % overlap,

    out[i] = fall[i] * src[prev + hop + i] + rise[i] * src[cur + i]

with a sin² window whose halves sum to exactly 1. `cur` moves by `hop * speed` per hop, then
by up to ±hop/4 to the offset whose start best matches what `prev` would have played next
(AMDF on mono, every 2nd sample, coarse then fine search). State is a few integers per voice;
the window table is shared.

**Integer maths only.** Positions and speeds are Q16.16, samples int16 with int32 mixing.
Float appears only at load time (the window table) and in the control path (fader positions),
never per sample.

**Every jump is a crossfade.** Cue, load, play, stop and switching between keylock and
varispeed all do the same thing: render the old "voice" and the new one for one block and
crossfade. A stopped voice renders silence, so starting and stopping are crossfades too.
There are no special-cased declick paths.

**Threads:** inputs, MIDI, touch and UI post events from one control task into a lock-free
single-producer queue; the audio task applies them at the start of each block. The UI reads
deck state through a seqlock.

Host tests (`tests/`) check pitch (zero crossings), speed, level, click-freedom and the
deck's transport. They also render the beat test at 0.8× and 1.25× to WAV: the kick spacing
comes out at 0.625 s and 0.399 s, as it should.

## 3. Screens: our own small renderer, u8g2 for the OLED controllers

The UI draws through a five-method `Surface` (size, mono or color, `fill`, and a "are these
rows visible?" query). One layout file covers 128x32 and 128x64 OLEDs and color TFTs, and the
same code renders to files on the host for tests and screenshots.

- **u8g2** drives the mono OLEDs (SSD1306, SH1106, ...): it owns the controller init and the
  1 KB frame buffer. It's one backend file, so it could be swapped out.
- **TFTs use ESP-IDF's `esp_lcd` directly, not LVGL.** LVGL would add 150-250 KB of code for a
  screen that is one fixed layout. It earns its place on menus, lists and animations, which
  is freedj-esp's territory. If nano ever needs those, LVGL comes in for the TFT builds only.
- **Fonts:** the built-in 5x7 font (scaled) looked blocky on the TFT. u8g2 ships hundreds of
  bitmap fonts in a compact run-length format; nano decodes that format itself (about 80
  lines in `gfx.cpp`) so the fonts go through the same renderer. Adobe Helvetica from X11,
  regular weight, 10 / 12 / 14 px and 24 px digits: +6 KB, +1 ms per frame. The OLEDs keep
  the 5x7 font, which is crisper at that size.

## 4. Driving a 320x240 TFT from a single-core C6

The plan was to fall back to a 128x64 OLED on the C6. The 3.2" TFT turned out to work well.

**No frame buffer.** A full RGB565 frame is 150 KB of the C6's 512 KB. Instead the screen is
drawn in bands into two DMA buffers: the CPU draws one band while SPI sends the other.

**The steps, each measured on the XIAO:**

| Step | UI per frame (playing) | fps |
|---|---|---|
| 16-row bands, 20 MHz SPI | 45 ms | 20 |
| Layout skips work outside the band (it ran in full once per band: 15x the work) | 31 → 17.6 ms | 28.5 (33 ms cap) |
| 48-row bands, 40 MHz SPI | 17.6 ms | 28.5 |
| Bands follow the layout's sections (waveform in one transfer) | 13.0 ms | 28.5 |
| Only changed rows are sent (hash per row); frame cap removed | 10.5 ms | **53** |

At 53 fps the waveform alone is 60 KB per frame, so SPI at 40 MHz is the limit. The XIAO's
pins go through the GPIO matrix, which caps SPI at 40 MHz; the C6's own SPI pins (6, 7) could
go faster on a board that exposes them. Hardware scrolling doesn't help, because the ILI9341
can only scroll the full height, not just the waveform section.

**Pins.** The A1S gets 7 fps out of the same panel because its screen lines are shared with
keys and LEDs, which capped SPI at 4 MHz. The XIAO's lines are free. On the XIAO, the TFT takes
the encoder's pins, so touch does jog and browse.

## 5. The ghosting chase

With the TFT running, copies of the moving waveform showed in the rows below it (the overview
and the top of the buttons), but not above it. The steps, in order:

1. **Tearing?** The panel refreshes on its own clock, and the waveform went out in strips that
   reached the panel a refresh apart. Sending the waveform in one transfer fixed a real ghost
   at the bottom of the waveform, but not this one.
2. **A buffer overwritten mid-transfer?** Ruled out by reading the driver: `esp_lcd` signals
   completion only after the last chunk, and each band's setup waits for the previous transfer.
3. **Strip skipping?** Re-sending every strip every frame (console `a`): no change.
4. **Power?** Moving the panel's VCC from 5 V to 3.3 V: no change.
5. **Panel voltages.** The same panel had run cleanly on the P4, which pointed away from the
   panel, but the ILI9341 driver's built-in init values are for TN panels and this is an IPS
   module (ILI9341V). Live tuning keys (`i` / `v` / `V` / `f`) switched to IPS values
   (C0 13, C1 13, C5 22 35, C7 BD): **gone.**

IPS panels (`CONFIG_NANO_TFT_INVERT`) now get those values at boot. Lessons:
- When in doubt, add a live knob rather than reflash per guess.
- The vendor's init sequence matters even when "the same driver worked before". The values used
  are a common IPS set, written from memory; they should be checked against the module
  vendor's init code.
- freedj-esp's A1S drives the same panel on the TN defaults, so it may show the same shadows.

## 6. Why one core is enough

Measured with the TFT running:

| | Average CPU | Worst block (share of its deadline) | Dropouts |
|---|---|---|---|
| 22.05 kHz mono, keylock | 5.0 % | | 0 |
| 44.1 kHz stereo, playing | 3.1 % | 4 % | 0 |
| 44.1 kHz stereo, keylock ±8 % | 16.4 % | 30-34 % | 0 |

Dropouts are counted by the I2S driver's send-queue-overflow event (the DMA came round to a
buffer that wasn't refilled and sent silence). The worst block is the slowest 256-frame render
of each 2 s window, against its 5.8 ms deadline at 44.1 kHz.

Why it fits:
- **The peripherals move the data.** I2S DMA streams audio (four buffers, ~23 ms of slack),
  SPI DMA streams pixels while the CPU draws the next band, the flash MMU maps the tracks,
  PCNT counts the encoder. The CPU only computes.
- **Nothing expensive runs.** No MP3 decoding, no LVGL, no file system, no float maths.
  Keylock costs about 600 cycles per stereo frame at 44.1 kHz.
- **Priority does the rest.** The audio task is highest priority. Every 5.8 ms it renders a
  block in at most ~2 ms, hands it to DMA and sleeps; the UI runs in the gaps and is pre-empted
  whenever audio needs the core. The screen pays for it: 53 fps drops to ~43 fps while keylock
  runs at 44.1 kHz. Audio never waits.

freedj-esp's second core exists to isolate heavy, bursty work (MP3 decoding, LVGL on large
screens, USB host, SD, networking) from the audio. Expected cost of adding each to nano:
- **microSD streaming:** fine; DMA does the reading. Needs a read-ahead cache for keylock's
  random access.
- **MP3 decoding:** possible; a fixed-point decoder is roughly 20-30 % at 44.1 kHz stereo.
  IMA ADPCM in the pack (4:1, block-wise so it stays seekable) is the cheaper route.
- **Wi-Fi Pro DJ Link:** the tightest fit.

There's also headroom left in the engine: keylock's search bounds-checks every sample read,
and a direct-pointer path would cut its cost roughly in half.

## 7. Tooling that made this fast

- **Host tests with outputs you can inspect:** WAV files to listen to and screenshots of every
  layout, from the same code the firmware runs.
- **Serial console over the C6's USB port** (`p` play, `c` cue, `k` keylock, `+` / `-` tempo,
  ...), so the deck can be driven with no buttons wired. Opening the port resets the C6, so
  keys sent while it boots are lost.
- **The DJ2Go through freedj-esp's MIDI bridge:** the controller on the computer, its MIDI
  forwarded over the same serial port (`0x01` + a USB-MIDI packet) to the same decoder.
- **A budget line every 2 s:** transport state, audio CPU, worst block, dropouts, UI ms per
  frame, KB sent per frame, free heap. Every change in the changelog carries its numbers.
- **`tools/build.sh`** builds all four configs in Docker in under a minute. It runs
  `reconfigure` first, because CMake only discovers new source files at configure time; without
  it, one flash quietly reused an old binary.
