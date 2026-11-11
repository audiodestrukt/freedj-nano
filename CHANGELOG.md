# freedj-nano changelog

Newest first. Each entry lists what changed, then the budget measured at that point.

## 2026-11-10: hw/keychain, a pocket deck enclosure for the XIAO ESP32-S3

- build123d CAD in the freedj-esp style (one params file, mate-placed parts, a numbered
  render log): 60 x 63.6 x 16.9 mm, CDJ layout. 0.95" SSD1331 colour OLED over a 32 mm jog
  (printed platter + 6 mm shaft, two MR106ZZ, magnet 1.0 mm over the AS5600 breakout),
  8 mm CUE over PLAY in the bottom-left corner with the platter's front edge in line with
  PLAY's, 35 mm pitch fader down the right side from the front edge. Inside: a main PCB
  outline (XIAO S3, PCM5102A, 3.5 mm jack, 6 x 6 tact switches, fader soldered through),
  402030 LiPo under it, USB-C out the back for a stick.
- `make` builds the STLs (shell, base, platter, collar, caps, knob, fit gauge; all single
  watertight bodies) and checks every pair of parts for overlap (28 parts, no clashes).
- To measure before printing (marked MEASURE in `params.py`): the OLED board and glass,
  the 35 mm fader's frame and lever, the jack, the LiPo. Open: the main PCB itself, and
  5 V for a stick on battery (needs a boost).

## 2026-11-10: hw/xiao-mini, a carrier board for the XIAO ESP32-C6 deck

- A 72 x 46 mm 2-layer board (tscircuit source, routed headless with the pcb-layout skill's
  route2: Freerouting, KiCad DRC under JLCPCB rules) that wires what the C6 firmware's XIAO
  OLED pin map expects: PCM5102A DAC -> 3.5 mm line out, OLED header, EC11 with push, four
  buttons on the ADC ladder (0 / 2k2 / 4k7 / 10k), MIDI-in header for the bridge, JST-PH LiPo
  socket. 0 shorts, 0 open, DRC and DFM clean; Gerbers, drill, CPL and BOMs in `fab/`.
- To check before ordering: the jack's pinout (no drawing found), the OLED module's pin order,
  an LCSC number for the 470 R output resistors.

## 2026-11-10: docs/CHIPS.md

- Chip options for the small decks, to come back to: what limits the CH32V003 (2 KB RAM, no
  I2S, no multiply, 16 pins, no USB host) and what it sounds like (mu-law noise, 22 kHz, mono);
  the CH32V002 / V006 / V203 / V30x and the RP2350 compared; five scenarios, from the V003 as
  is, to a V006 with 4x the RAM, to a CH32V203 + PCM5102A at 16-bit stereo 44.1 kHz with the
  DJ2Go on its USB host (no I2S on the V203, so SPI + a timer for the DAC's clocks).

## 2026-11-10: docs/DECK10C-CONTROLS.md (undecided)

- The CH32V003 deck's controls, written up to come back to: all 16 usable pins are taken (the
  pot goes on PD5, the unused USART TX: DECK10C's pin table updated). Jog options compared:
  EC11 (now), AS5600 on the OLED's I2C bus (no pins), a slotted 3D-printed wheel with two slot
  sensors counted by TIM2's encoder mode on PD3 / PD4 (no CPU, ~$0.20-0.60, 4N counts a turn),
  with how to design the wheel; a side thumbwheel for browsing; buttons on a binary-weighted
  resistor ladder to free pins; pin plans for each; a one-turn test to learn the DJ2Go's jog.

## 2026-11-10: CH32V003 scrub audio, both ways; cue mark on the overview

- nano-lite plays backwards: a negative speed (varispeed only). When a backwards read falls
  below the 35 ms ring, the ring is refilled with the playhead near its top, one forward read
  from the card: one jump per ~700 frames. New tests: a 440 Hz tone backwards at 1x, 2.5x,
  0.3x (pitch, travel, no misses, no clicks), a back-and-forth scratch. 106 checks.
- The deck: turning the jog while paused plays what's under it, either way, at the speed it
  turns (chasing the jog's position over ~23 ms, up to 4x), keylock off while scrubbing; 80 ms
  after the jog stops it fades out and lands exactly where the jog put it. PLAY plays on from
  the scrub; CUE sets the cue there. Sim checks: scrub forward lands exactly and is audible,
  scrub back plays a 440 Hz tone at the expected pitch. 36 checks.
- Overview (page 7): bars in the bottom 6 rows, a triangle at the cue in a 2-row lane above,
  so it shows whatever the music looks like.
- `make deckbench` with a ~2.7x back-and-forth scratch: worst scratch step 129 % of the ring
  (refills are card jumps; with the waveform's). Overall worst still 135 % (waveform + keylock
  search). RAM 2007 of 2048 B, flash ~13.8 of 16 KB.

## 2026-11-10: docs/DECK10C-PERF.md

- The CH32V003 deck's performance written up for coming back to: the audio cushion (4.35 ms =
  209 k cycles) and why the worst step matters more than average CPU, why the SD card's jumps
  are the slow part, cycles per instruction, the optimizations done (with numbers) and six to
  try (waveform RAM cache, no two expensive things per step, a cheaper list, non-blocking SD
  reads, SPI NOR flash, small stuff), and how to measure each.

## 2026-11-10: CH32V003 scrolling waveform; a cycle model; CPU and fps in the sim

- Screen pages 4-6: a CDJ-style waveform, 1.5 s around the playhead (0.37 s behind), 23 px
  mirrored, at least 1 px so it joins up, beat ticks (downbeats longer), playhead inverted, cue
  notch, the past dimmed. One peak per 256 frames in TRACKS.BIN (pack_lite.py, `peaks_off` in
  the entry's spare bytes); read from the card straight into the page buffer, so no RAM.
  Pages are addressed one at a time (`WAVE_DIV` can skip the waveform on some frames). Page 3
  now carries the cue time; page 7 an 8 px overview. The bar meter and elapsed time went.
- RAM: Petit FatFs's state borrows the screen's page buffer during the mount: 1998 of 2048 B
  (50 B spare). The mount waits for the I2C to be idle.
- `make deckbench`: a cycle model (flash fetch at one wait state, +1 per load / store, +2 per
  taken branch, 20 cycles per SPI byte) next to the flat 1.5: 2.57 cycles per instruction.
  37 % CPU, worst step 135 % of the ring: **the waveform's SD reads overrun the audio** (two
  stream restarts in a step); without the waveform 22 % and 91 %. Worst steps broken down by
  part; the boot mount (silent) left out.
- `make sim` window: CPU estimate (per-unit costs from the cycle model: 35.5 % vs deckbench's
  37 % on the same session; it underestimates the worst step), worst step, fps.
- Budget: flash 12.1 KB + ~1.5 KB drivers = ~13.3 of 16 KB. 31 sim checks pass.

## 2026-11-10: CH32V003 deck on a 128x64 OLED

- The screen is now the 0.96" SSD1306 128x64 (more common, more room). Same one-page buffer, so
  no more RAM. Layout: name; time left at double size with BPM at tempo and tempo; PLAY / PAUSE /
  CUE, an inverted KEY tag, the range; a four-beat bar meter from the beat grid; elapsed and the
  cue point; the 16 px overview. The list: a "TRACKS n/m" header and 7 names. The emulator
  follows the multiplex ratio (64 or 32 rows).
- Budget (`make deckbench`): flash 11.5 KB + ~1.5 KB drivers = ~12.7 of 16 KB (+0.6), RAM 2027
  of 2048 B (+4: the first beat), CPU 15 % (+0.5), worst step 105 k instructions = 75 % of the
  ring (the list, was 68 %). 31 sim checks pass.

## 2026-11-10: the whole CH32V003 deck, simulated

- `ch32v003/fw/`: the deck firmware behind `hal.h`: CDJ transport (CUE back-to-cue / set and
  preview, PLAY), EC11 jog (nudge / search), SHIFT + turn list, push keylock, SHIFT + push range,
  pitch pot, the DJ2Go over the bridge (0x01 + USB-MIDI) and console keys, SSD1306 pages (name;
  time left, BPM at tempo, tempo, keylock, state; 16 px overview), SD over SPI with streaming
  reads, TRACKS.BIN (`tools/pack_lite.py`), Petit FatFs (vendored, mount and open only, plus
  `pf_contig()`).
- `ch32v003/sim/`: the firmware on emulated parts (SD card on SPI, SSD1306 on I2C), in an SDL
  window with sound, the panel on the keyboard, a pty for `midi_bridge.py` or the DJ2Go direct
  (`make sim`); headless scripted sessions, 31 checks (`make simtest`); `tools/sdimage.py`
  writes FAT32 card images (MBR, 32 KB clusters, `--fragment`).
- `make deckbench`: the same firmware for RV32EC in qemu, every instruction counted by part.
- Budget (10.5 s session, 1.5 cycles per instruction): flash 10.8 KB + ~1.5 KB drivers = ~12.1 of
  16 KB; RAM 1403 B + 256 B DMA ring + 364 B stack = 2023 of 2048 B; CPU 14 % (engine 9 %); worst
  main-loop step 95 k instructions, 68 % of the ring (the list view). Single-block SD reads
  instead: 66 % CPU, worst step 303 %: streaming (CMD18) is required.

## 2026-11-10: CH32V003 PWM output modelled

- `ch32v003/sim/pwm_sim.py` (`make pwm`): TIM1's two PWM pins, the 1 : 256 resistor sum (with
  tolerances and the pins' output resistance) and the filter, solved exactly per PWM period from
  the circuit's state-space model, period-averaged and decimated to 44.1 kHz. Checked against
  tick-by-tick stepping (4e-14 V) and ngspice AC (0.000 dB). Prints THD+N, images and ripple per
  variant and renders a track through each for listening.
- Found: edge-aligned PWM (the plan) has a 2nd harmonic at -48 dB at 1 kHz, from pulse centres
  moving with duty; centre-aligned at 88.2 kHz is -76 dB. R2 267k (256 x (1k + ~40 ohm pin))
  instead of 255k: -43.6 vs -36.3 dB THD+N at -40 dBFS. A pole + Q = 1 Sallen-Key on the MCP6002:
  ripple 55 -> 6 mVpp. Every dual-PWM variant beats the mu-law track (-37.6 dB); one 8-bit pin
  is -14 dB at -40 dBFS. Left to hear: the 22.05 kHz images (12.05 kHz image of a 10 kHz tone
  at -5 dB); 2x interpolation on the chip takes the 5 kHz one from -26 to -35 dB.
- docs/DECK10C.md: the results table, centre-aligned TIM1 timing (48 MHz / 544, repetition
  counter 7 = 22,059 Hz), filter values, open question now the images, not dual PWM.

## 2026-11-10: CH32V003 phase 0, nano-lite fits

- `ch32v003/`: nano-lite, the deck engine in C for the 10-cent chip (RV32EC, no multiply, 2 KB
  RAM). Streams a mono 22.05 kHz mu-law track through a 768-byte ring plus a 192-byte 4:1
  decimated copy for the keylock's coarse search; varispeed; keylock with the +-1 hop search,
  clamped so every read fits in the rings; 1.5 ms fades on jumps, starts, stops and engine
  switches. Host tests (83 checks, some against the C6 engine) and an RV32EC bench: the ESP-IDF
  image's GCC for `rv32ec`/`ilp32e`, qemu-riscv32 `rv32e,c=true` counting every instruction.
- Budget (22.05 kHz mono, instructions per sample; CPU at an assumed 1.5 cycles each, 48 MHz):
  varispeed 87 (1.00) / 144 (1.08), 6-10 %; keylock at 1.08 257 average, 18 %, worst block 911,
  63 % of real time (a 4 x 32-sample DMA ring absorbs it). Engine 3.7 KB flash (3.1 KB code,
  0.6 KB mu-law table), 1032 B RAM, no multiply or divide instructions.
- Got there from 613 instructions per sample with the search block at 222 % of real time: one
  fetch per search and direct ring reads (was a checked read per sample), centre-out scanning
  (ties went to the most negative offset), forced inlining under -Os, coarse every 8th offset.
- docs/DECK10C.md: phase 0 results, measured budgets, the PWM timing (48 MHz / 272, a sample every
  8 periods = 22,059 Hz).

## 2026-11-10: keylock holds the bass's pitch

- Heard on the bench: keylock "changes the pitch a little". Measured on test tones at +8 %:
  everything under ~200 Hz came out 7.7-7.9 % sharp (it followed the tempo, as varispeed),
  above 220 Hz within 0.04 %. The search was +-hop/4 (2.9 ms), but a bass note needs up to half
  its period to line up (10 ms at 50 Hz). Now +-1 hop (11.6 ms) in two passes: coarse, every 8th
  offset (4th at 22.05 kHz) comparing a whole hop sampled at that stride; then fine, every offset
  within one coarse step. 45-1000 Hz now within 0.25 % at -8 %, +8 % and +16 %. New test: 50, 80,
  120 Hz at +8 %, 22.05 and 44.1 kHz (fails on the old search: 50 Hz played at 54 Hz).
- Budget (XIAO, 22.05 kHz mono, keylock at +8 %): audio 8 % of the core (was 3-5 %), worst block
  13 % of its deadline (was 5-7 %), 0 dropouts. 44.1 kHz stereo not re-measured (was 16 %).
- freedj-esp's WSOLA has the same +-2.9 ms search (SEARCH = 128 at 44.1 kHz).

## 2026-11-10: first sound (PCM5102A on the XIAO)

- Heard on a PCM5102A breakout on the XIAO TFT build (BCK D1, LRCK D2, DIN D6, VIN 5V, FMT
  set to I2S). Only the left channel played at first: with 32-bit slots for 16-bit samples,
  ESP-IDF's Philips default sets the WS width to the data width (16 BCKs), so WS flipped mid-slot
  and the DAC never framed the right channel. `ws_width = 32` fixes it, and it sounds cleaner too
  (the left channel was likely misframed as well).
- Budget: playing at speed 1, audio 1.4-1.5 % of the core, worst block 2 % of its deadline,
  0 dropouts; UI 15.9 ms/frame at 62 KB/frame.

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
