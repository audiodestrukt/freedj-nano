# The 10-cent DJ deck (plan)

Status: plan, 2026-11-09. Nothing built yet. A DJ deck around the **CH32V003**, a 10-cent
RISC-V microcontroller with 2 KB of RAM. The point is to find out how far freedj-nano's
no-buffers engine stretches; the board around it costs a few dollars. Context: why this chip and
not the others in the cheap-MCU roundup is in [TARGETS.md](TARGETS.md).

**Goal:** play a track from flash with varispeed and a jog; CUE and PLAY as on a CDJ; a 128x32
OLED; DJ2Go control. **Stretch goal:** keylock.

## The chip

CH32V003F4P6 (TSSOP-20), the part already on the freedj control board:

| | |
|---|---|
| Core | QingKe V2A, RV32EC, 48 MHz. **No hardware multiply** |
| Memory | 16 KB flash, 2 KB SRAM |
| Peripherals | TIM1 (advanced, 4-channel PWM), TIM2, SPI, I2C, USART, 10-bit ADC, 7-channel DMA |
| Programming | SWIO, one wire on PD1, from a **WCH-LinkE** (~$5); no USB, no factory bootloader |

## Hardware

| Part | Choice | Notes |
|---|---|---|
| MCU | CH32V003F4P6 dev board | buy the listing that includes the WCH-LinkE |
| Tracks | **microSD in SPI mode** (default) | a whole library; files written on the computer. Read with ChaN's **Petit FatFs** (read-only FAT, ~2-4 KB code, ~40 B RAM, partial-sector reads: no 512-byte buffer) |
| Tracks, fallback | W25Q128 SPI NOR flash, 16 MB | if SD latency spikes turn out to matter: NOR reads start in microseconds. Written by a CH341A programmer with `flashrom` |
| Audio out | **PWM** from TIM1 + RC filter | see below; mono |
| Amp / line out | small op-amp line driver, or a PAM8302 for a speaker | |
| Screen | **SSD1306 128x32** (0.91") on I2C | 512 bytes per frame, drawn a page at a time |
| Controls | EC11 encoder with push (jog / browse), PLAY, CUE, SHIFT buttons, a 10k pitch pot | |
| MIDI | the USART | DJ2Go through freedj-esp's `midi_bridge.py`, over the WCH-LinkE's own serial port (no adapter needed); DIN MIDI with an optocoupler later |

### Audio: PWM

No I2S, so audio is PWM from TIM1 with a low-pass filter. 8 bits at a 187.5 kHz carrier
(48 MHz / 256) is easy to filter but noisy. Better: **two 8-bit channels summed through resistors
weighted 1 : 256** (TIM1 CH1 for the high byte, CH2 for the low byte), which gives about 14-16 bits
of usable resolution at the same carrier. The sample rate is 22.05 kHz; DMA from a small
buffer updates the duty cycles, so the CPU doesn't take an interrupt per sample.

### Draft pins (check against the datasheet before wiring)

| Function | Pin |
|---|---|
| SD card (or SPI flash) SCK / MOSI / MISO / CS | PC5 / PC6 / PC7 / PC0 |
| OLED I2C SDA / SCL | PC1 / PC2 |
| PWM audio high / low byte | PD2 (TIM1 CH1) / PA1 (TIM1 CH2) |
| EC11 A / B / push | PD3 / PD4 / PC3 |
| PLAY / CUE / SHIFT | PC4 / PA2 / PD0 (or a resistor ladder on one ADC pin) |
| Pitch pot | an ADC pin (ladder and pot share the ADC) |
| USART RX (MIDI, bridge) / TX | PD6 / PD5 |
| SWIO | PD1 |

## Shrinking the engine

nano's engine (`components/nano`) is C++ with atomics, Q16 positions and a 1 KB window table.
On the CH32V003 it becomes a small C port, "nano-lite", that keeps the same ideas:

- **Tracks streamed forward through a small ring, not memory mapping.** Everything the engine
  reads stays near the playhead: playback reads straight ahead, and keylock's two overlapping
  frames and its ±64-sample search together span about 2 hops + the search, ~576 samples at
  22.05 kHz. So one ring of ~1 KB (16-bit) or ~576 B (8-bit µ-law), refilled by sequential
  multi-block SD reads (CMD18), covers it with no random seeks. Only jumps (cue, load, a long
  scratch back) seek, with a short gap, as on a CDJ.
- **Linear crossfade instead of the sin² table.** WSOLA's two halves still sum to exactly 1, the
  weights step by addition, and the 512-byte table goes. Slightly more audible at the seams; listen
  and compare in the host tests.
- **Multiplies only where they're needed.** Software multiply costs ~30-100 cycles on RV32EC. Two
  per sample for the crossfade; varispeed interpolation by an 8-bit fraction (shift and add), or
  nearest-sample at speed 1.0.
- **8-bit decimated search.** The AMDF search compares a downsampled 8-bit copy of the window:
  256 bytes, and cheaper comparisons.
- **Same track data, one file per track.** On SD, `pack_tracks.py` writes each track as its own
  file (the pack entry's PCM, peaks and grid), 22.05 kHz mono, 44 KB/s at 16 bits; on NOR flash it's
  the same pack image as the C6's. The peak table gives the OLED its waveform strip; rekordbox grids
  work as on the C6. MP3s from a rekordbox SD export can't play directly: no decoder fits.

### Budgets (estimates: phase 0 measures them)

| | Estimate |
|---|---|
| **CPU**, varispeed | ~5 % of 48 MHz (one PWM DMA refill per block, a shift-add interpolation per sample) |
| **CPU**, keylock | ~15-25 %: 2 software multiplies plus ~10 abs-diffs of search per output sample |
| **RAM** | stack 512 B, OLED page 128 B, track ring ~576 B (µ-law) to 1 KB (16-bit), PWM DMA buffer 2 x 64 B, deck state ~100 B, Petit FatFs ~40 B: **~1.5-1.9 of 2 KB** (µ-law gives the margin) |
| **Flash** | engine ~4 KB, OLED page renderer + 5x7 font ~2 KB, SSD1306 + SD + PWM drivers ~2 KB, Petit FatFs ~2-4 KB, inputs ~1 KB, MIDI parser + DJ2Go map ~1.5 KB: **~12-15 of 16 KB**: tight; phase 0 checks it |
| **Screen** | 512 bytes per frame over I2C at 400 kHz (~13 ms) or 1 MHz (~5 ms) with DMA: 20-30 fps, in the gaps |

## Phases

0. **Measure on the computer.** Write nano-lite in C, test it on the host against the C6 engine
   (pitch, speed, clicks, the WAV files), then compile it for RV32EC (`-march=rv32ec_zicsr
   -mabi=ilp32e`) and count cycles per sample in an instruction-set simulator. Decides: keylock
   or varispeed-only, and whether the flash budget holds.
1. **Bring-up.** WCH-LinkE + `minichlink` (ch32fun), blink, then a PWM test tone through the RC
   filter. Listen to 8-bit against the dual-PWM 16-bit.
2. **Play a track.** Mount the SD card with Petit FatFs, stream a track file through the ring,
   play it at speed 1 and with varispeed. Log the longest read stall per card: that decides SD vs
   the NOR fallback.
3. **Screen.** SSD1306 page renderer: time remaining, tempo, a 16-pixel waveform strip.
4. **Controls.** EC11 (jog: nudge / scrub; with SHIFT: browse), PLAY, CUE, the pitch pot, and the
   DJ2Go over the bridge.
5. **Keylock,** if phase 0 says it fits.
6. **A board.** A small PCB in the style of `ctrl-pcb` (tscircuit, JLCPCB-stocked parts): CH32V003,
   a microSD socket, the PWM filter, a 3.5 mm jack, the OLED header, the encoder and buttons.

## Shopping list

- CH32V003F4P6 dev boards, **with the WCH-LinkE**
- microSD breakout for 3.3 V (SPI), and a small, fast card (any A1-rated card)
- (fallback) W25Q128 SPI flash breakout and a CH341A programmer with a SOIC-8 clip
- SSD1306 0.91" 128x32 I2C OLED
- EC11 encoder with push switch, 3 tact switches, a 10k pot
- Resistors and capacitors for the dual-PWM sum and the RC filter (2nd order, ~10 kHz cut-off);
  an op-amp (MCP6002) as a line driver

## Where it lives

`ch32v003/` in freedj-nano: a ch32fun Makefile build, the nano-lite engine in C, and the drivers.
It shares the pack format, `pack_tracks.py`, the rekordbox reader, the DJ2Go map and the host
tests with the C6 build.

## Open questions

- SD read stalls: how long, how often, per card (phase 2 logs it)? A ~1 KB ring holds ~23 ms of
  16-bit audio at 22.05 kHz, or ~26 ms of µ-law, against the occasional 10 ms+ stall.
- Does Petit FatFs fit next to everything else in 16 KB?
- Does keylock fit (phase 0)? If it's borderline, an 11.025 kHz keylock mode is a fallback.
- Is dual-PWM good enough to DJ with, or does this deck want a cheap I2S DAC? There's no I2S
  peripheral; bit-banged I2S at 22 kHz would cost most of the CPU.
- 8-bit µ-law in the pack: twice the music in the same flash, one table lookup per sample.
- Stereo: TIM1 has four channels, enough for two dual-PWM outputs; costs RAM and CPU.
